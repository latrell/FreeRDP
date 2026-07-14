/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Dynamic Virtual Channel
 *
 * Copyright 2010-2011 Vic Lee
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <winpr/crt.h>
#include <winpr/cast.h>
#include <winpr/stream.h>
#include <winpr/interlocked.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/utils/drdynvc.h>
#include <freerdp/codec/zgfx.h>

#include "drdynvc_main.h"

#define TAG CHANNELS_TAG("drdynvc.client")

static const char* channel_state2str(DVC_CHANNEL_STATE state)
{
	switch (state)
	{
		case DVC_CHANNEL_INIT:
			return "DVC_CHANNEL_INIT";
		case DVC_CHANNEL_RUNNING:
			return "DVC_CHANNEL_RUNNING";
		case DVC_CHANNEL_CLOSED:
			return "DVC_CHANNEL_CLOSED";
		default:
			return "DVC_CHANNEL_UNKNOWN";
	}
}

static void dvcman_channel_free(DVCMAN_CHANNEL* channel);
static UINT dvcman_channel_close(DVCMAN_CHANNEL* channel, BOOL perRequest, BOOL fromHashTableFn);
static void dvcman_free(drdynvcPlugin* drdynvc, IWTSVirtualChannelManager* pChannelMgr);
static UINT drdynvc_write_data(drdynvcPlugin* drdynvc, DVCMAN_CHANNEL* channel, const BYTE* data,
                               UINT32 dataSize, BOOL* close, DVCMAN_CHANNEL_STATS* stats);
static UINT drdynvc_send(drdynvcPlugin* drdynvc, wStream* s, DVCMAN_CHANNEL_STATS* stats);

enum
{
	DRDYNVC_QUEUE_TCP_PDU = 0,
	DRDYNVC_QUEUE_UDP_PDU = 1
};

typedef struct
{
	wStream* data;
	UINT32 tunnelType;
	UINT32 channelId;
	UINT64 generation;
	UINT8 command;
	BOOL bindGeneration;
} DRDYNVC_UDP_QUEUE_ITEM;

static void dvcman_wtslistener_free(DVCMAN_LISTENER* listener)
{
	if (listener)
		free(listener->channel_name);
	free(listener);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_get_configuration(IWTSListener* pListener, void** ppPropertyBag)
{
	WINPR_ASSERT(ppPropertyBag);
	WINPR_UNUSED(pListener);
	*ppPropertyBag = nullptr;
	return ERROR_INTERNAL_ERROR;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_create_listener(IWTSVirtualChannelManager* pChannelMgr,
                                   const char* pszChannelName, ULONG ulFlags,
                                   IWTSListenerCallback* pListenerCallback,
                                   IWTSListener** ppListener)
{
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;
	DVCMAN_LISTENER* listener = nullptr;

	WINPR_ASSERT(dvcman);
	WLog_DBG(TAG, "create_listener: %" PRIuz ".%s.", HashTable_Count(dvcman->listeners) + 1,
	         pszChannelName);
	listener = (DVCMAN_LISTENER*)calloc(1, sizeof(DVCMAN_LISTENER));

	if (!listener)
	{
		WLog_ERR(TAG, "calloc failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	listener->iface.GetConfiguration = dvcman_get_configuration;
	listener->iface.pInterface = nullptr;
	listener->dvcman = dvcman;
	listener->channel_name = _strdup(pszChannelName);

	if (!listener->channel_name)
	{
		WLog_ERR(TAG, "_strdup failed!");
		dvcman_wtslistener_free(listener);
		return CHANNEL_RC_NO_MEMORY;
	}

	listener->flags = ulFlags;
	listener->listener_callback = pListenerCallback;

	if (ppListener)
		*ppListener = (IWTSListener*)listener;

	if (!HashTable_Insert(dvcman->listeners, listener->channel_name, listener))
	{
		dvcman_wtslistener_free(listener);
		return ERROR_INTERNAL_ERROR;
	}

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc): HashTable_Insert takes ownership of listener
	return CHANNEL_RC_OK;
}

static UINT dvcman_destroy_listener(IWTSVirtualChannelManager* pChannelMgr, IWTSListener* pListener)
{
	DVCMAN_LISTENER* listener = (DVCMAN_LISTENER*)pListener;

	WINPR_UNUSED(pChannelMgr);

	if (listener)
	{
		DVCMAN* dvcman = listener->dvcman;
		if (dvcman)
			HashTable_Remove(dvcman->listeners, listener->channel_name);
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_register_plugin(IDRDYNVC_ENTRY_POINTS* pEntryPoints, const char* name,
                                   IWTSPlugin* pPlugin)
{
	WINPR_ASSERT(pEntryPoints);
	DVCMAN* dvcman = ((DVCMAN_ENTRY_POINTS*)pEntryPoints)->dvcman;

	WINPR_ASSERT(dvcman);
	if (!ArrayList_Append(dvcman->plugin_names, name))
		return ERROR_INTERNAL_ERROR;
	if (!ArrayList_Append(dvcman->plugins, pPlugin))
		return ERROR_INTERNAL_ERROR;

	WLog_DBG(TAG, "register_plugin: num_plugins %" PRIuz, ArrayList_Count(dvcman->plugins));
	return CHANNEL_RC_OK;
}

static IWTSPlugin* dvcman_get_plugin(IDRDYNVC_ENTRY_POINTS* pEntryPoints, const char* name)
{
	IWTSPlugin* plugin = nullptr;
	size_t nc = 0;
	size_t pc = 0;
	WINPR_ASSERT(pEntryPoints);
	DVCMAN* dvcman = ((DVCMAN_ENTRY_POINTS*)pEntryPoints)->dvcman;
	if (!dvcman || !pEntryPoints || !name)
		return nullptr;

	nc = ArrayList_Count(dvcman->plugin_names);
	pc = ArrayList_Count(dvcman->plugins);
	if (nc != pc)
		return nullptr;

	ArrayList_Lock(dvcman->plugin_names);
	ArrayList_Lock(dvcman->plugins);
	for (size_t i = 0; i < pc; i++)
	{
		const char* cur = ArrayList_GetItem(dvcman->plugin_names, i);
		if (strcmp(cur, name) == 0)
		{
			plugin = ArrayList_GetItem(dvcman->plugins, i);
			break;
		}
	}
	ArrayList_Unlock(dvcman->plugin_names);
	ArrayList_Unlock(dvcman->plugins);
	return plugin;
}

static const ADDIN_ARGV* dvcman_get_plugin_data(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	WINPR_ASSERT(pEntryPoints);
	return ((DVCMAN_ENTRY_POINTS*)pEntryPoints)->args;
}

static rdpContext* dvcman_get_rdp_context(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	DVCMAN_ENTRY_POINTS* entry = (DVCMAN_ENTRY_POINTS*)pEntryPoints;
	WINPR_ASSERT(entry);
	return entry->context;
}

static rdpSettings* dvcman_get_rdp_settings(IDRDYNVC_ENTRY_POINTS* pEntryPoints)
{
	rdpContext* context = dvcman_get_rdp_context(pEntryPoints);
	WINPR_ASSERT(context);

	return context->settings;
}

static UINT32 dvcman_get_channel_id(IWTSVirtualChannel* channel)
{
	DVCMAN_CHANNEL* dvc = (DVCMAN_CHANNEL*)channel;
	WINPR_ASSERT(dvc);
	return dvc->channel_id;
}

static const char* dvcman_get_channel_name(IWTSVirtualChannel* channel)
{
	DVCMAN_CHANNEL* dvc = (DVCMAN_CHANNEL*)channel;
	WINPR_ASSERT(dvc);
	return dvc->channel_name;
}

static DVCMAN_CHANNEL* dvcman_get_channel_by_id(IWTSVirtualChannelManager* pChannelMgr,
                                                UINT32 ChannelId, BOOL doRef)
{
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;
	DVCMAN_CHANNEL* dvcChannel = nullptr;

	WINPR_ASSERT(dvcman);
	HashTable_Lock(dvcman->channelsById);
	dvcChannel = HashTable_GetItemValue(dvcman->channelsById, &ChannelId);
	if (dvcChannel)
	{
		if (doRef)
			InterlockedIncrement(&dvcChannel->refCounter);
	}

	HashTable_Unlock(dvcman->channelsById);
	return dvcChannel;
}

static UINT32 drdynvc_udp_tunnel_mask(UINT32 tunnelType)
{
	switch (tunnelType)
	{
		case TUNNELTYPE_UDPFECR:
			return DRDYNVC_UDP_TUNNEL_MASK_FECR;

		case TUNNELTYPE_UDPFECL:
			return DRDYNVC_UDP_TUNNEL_MASK_FECL;

		default:
			return 0;
	}
}

static drdynvcPlugin* drdynvc_get_plugin_from_context(DrdynvcClientContext* context)
{
	if (!context || !context->handle)
		return nullptr;

	drdynvcPlugin* drdynvc = (drdynvcPlugin*)context->handle;
	if ((drdynvc->context != context) || !drdynvc->udpLockInitialized ||
	    !drdynvc->udpReceiveLockInitialized)
		return nullptr;

	return drdynvc;
}

static BOOL drdynvc_udp_clear_route(WINPR_ATTR_UNUSED const void* key, void* value,
                                    WINPR_ATTR_UNUSED void* arg)
{
	DVCMAN_CHANNEL* channel = (DVCMAN_CHANNEL*)value;
	WINPR_ASSERT(channel);
	channel->udpTunnelType = 0;
	return TRUE;
}

static void drdynvc_clear_udp_routes_locked(drdynvcPlugin* drdynvc)
{
	WINPR_ASSERT(drdynvc);

	if (drdynvc->channel_mgr)
	{
		DVCMAN* dvcman = (DVCMAN*)drdynvc->channel_mgr;
		(void)HashTable_Foreach(dvcman->channelsById, drdynvc_udp_clear_route, nullptr);
	}

	drdynvc->udpNegotiatedMask = 0;
	drdynvc->softSyncActive = FALSE;
	drdynvc->udpGeneration++;
}

static void drdynvc_clear_udp_transport_locked(drdynvcPlugin* drdynvc)
{
	WINPR_ASSERT(drdynvc);
	drdynvc_clear_udp_routes_locked(drdynvc);
	drdynvc->udpSend = nullptr;
	drdynvc->udpUserData = nullptr;
	drdynvc->udpTunnelMask = 0;
}

UINT drdynvc_set_udp_transport(DrdynvcClientContext* context, pcDrdynvcUdpSend send, void* userData,
                               UINT32 tunnelMask)
{
	drdynvcPlugin* drdynvc = drdynvc_get_plugin_from_context(context);
	if (!drdynvc || !send || ((tunnelMask & ~DRDYNVC_UDP_TUNNEL_MASK_ALL) != 0))
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&drdynvc->udpLock);
	if (drdynvc->softSyncActive &&
	    ((tunnelMask & drdynvc->udpNegotiatedMask) != drdynvc->udpNegotiatedMask))
	{
		LeaveCriticalSection(&drdynvc->udpLock);
		return ERROR_INVALID_STATE;
	}

	drdynvc->udpSend = send;
	drdynvc->udpUserData = userData;
	drdynvc->udpTunnelMask = tunnelMask;
	LeaveCriticalSection(&drdynvc->udpLock);
	return CHANNEL_RC_OK;
}

void drdynvc_clear_udp_transport(DrdynvcClientContext* context)
{
	drdynvcPlugin* drdynvc = drdynvc_get_plugin_from_context(context);
	if (!drdynvc)
		return;

	EnterCriticalSection(&drdynvc->udpReceiveLock);
	EnterCriticalSection(&drdynvc->udpLock);
	drdynvc_clear_udp_transport_locked(drdynvc);
	LeaveCriticalSection(&drdynvc->udpLock);
	LeaveCriticalSection(&drdynvc->udpReceiveLock);
}

static IWTSVirtualChannel* dvcman_find_channel_by_id(IWTSVirtualChannelManager* pChannelMgr,
                                                     UINT32 ChannelId)
{
	DVCMAN_CHANNEL* channel = dvcman_get_channel_by_id(pChannelMgr, ChannelId, FALSE);
	if (!channel)
		return nullptr;

	return &channel->iface;
}

static void dvcman_plugin_terminate(void* plugin)
{
	IWTSPlugin* pPlugin = plugin;

	WINPR_ASSERT(pPlugin);
	UINT error = IFCALLRESULT(CHANNEL_RC_OK, pPlugin->Terminated, pPlugin);
	if (error != CHANNEL_RC_OK)
		WLog_ERR(TAG, "Terminated failed with error %" PRIu32 "!", error);
}

static void wts_listener_free(void* arg)
{
	DVCMAN_LISTENER* listener = (DVCMAN_LISTENER*)arg;
	dvcman_wtslistener_free(listener);
}

static BOOL channelIdMatch(const void* k1, const void* k2)
{
	WINPR_ASSERT(k1);
	WINPR_ASSERT(k2);
	return *((const UINT32*)k1) == *((const UINT32*)k2);
}

static UINT32 channelIdHash(const void* id)
{
	WINPR_ASSERT(id);
	return *((const UINT32*)id);
}

static void channelByIdCleanerFn(void* value)
{
	DVCMAN_CHANNEL* channel = (DVCMAN_CHANNEL*)value;
	if (channel)
	{
		dvcman_channel_close(channel, FALSE, TRUE);
		dvcman_channel_free(channel);
	}
}

static IWTSVirtualChannelManager* dvcman_new(drdynvcPlugin* plugin)
{
	wObject* obj = nullptr;
	DVCMAN* dvcman = (DVCMAN*)calloc(1, sizeof(DVCMAN));

	if (!dvcman)
		return nullptr;

	dvcman->iface.CreateListener = dvcman_create_listener;
	dvcman->iface.DestroyListener = dvcman_destroy_listener;
	dvcman->iface.FindChannelById = dvcman_find_channel_by_id;
	dvcman->iface.GetChannelId = dvcman_get_channel_id;
	dvcman->iface.GetChannelName = dvcman_get_channel_name;
	dvcman->drdynvc = plugin;
	dvcman->channelsById = HashTable_New(TRUE);

	if (!dvcman->channelsById)
		goto fail;

	if (!HashTable_SetHashFunction(dvcman->channelsById, channelIdHash))
		goto fail;

	obj = HashTable_KeyObject(dvcman->channelsById);
	WINPR_ASSERT(obj);
	obj->fnObjectEquals = channelIdMatch;

	obj = HashTable_ValueObject(dvcman->channelsById);
	WINPR_ASSERT(obj);
	obj->fnObjectFree = channelByIdCleanerFn;

	dvcman->pool = StreamPool_New(TRUE, 10);
	if (!dvcman->pool)
		goto fail;

	dvcman->listeners = HashTable_New(TRUE);
	if (!dvcman->listeners)
		goto fail;

	if (!HashTable_SetHashFunction(dvcman->listeners, HashTable_StringHash))
		goto fail;

	obj = HashTable_KeyObject(dvcman->listeners);
	obj->fnObjectEquals = HashTable_StringCompare;

	obj = HashTable_ValueObject(dvcman->listeners);
	obj->fnObjectFree = wts_listener_free;

	dvcman->plugin_names = ArrayList_New(TRUE);
	if (!dvcman->plugin_names)
		goto fail;
	obj = ArrayList_Object(dvcman->plugin_names);
	obj->fnObjectNew = winpr_ObjectStringClone;
	obj->fnObjectFree = winpr_ObjectStringFree;

	dvcman->plugins = ArrayList_New(TRUE);
	if (!dvcman->plugins)
		goto fail;
	obj = ArrayList_Object(dvcman->plugins);
	obj->fnObjectFree = dvcman_plugin_terminate;
	return &dvcman->iface;
fail:
	dvcman_free(plugin, &dvcman->iface);
	return nullptr;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_load_addin(drdynvcPlugin* drdynvc, IWTSVirtualChannelManager* pChannelMgr,
                              const ADDIN_ARGV* args, rdpContext* context)
{
	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(pChannelMgr);
	WINPR_ASSERT(args);
	WINPR_ASSERT(context);

	WLog_Print(drdynvc->log, WLOG_INFO, "Loading Dynamic Virtual Channel %s", args->argv[0]);

	PVIRTUALCHANNELENTRY pvce = freerdp_load_channel_addin_entry(args->argv[0], nullptr, nullptr,
	                                                             FREERDP_ADDIN_CHANNEL_DYNAMIC);
	PDVC_PLUGIN_ENTRY pDVCPluginEntry = WINPR_FUNC_PTR_CAST(pvce, PDVC_PLUGIN_ENTRY);

	if (pDVCPluginEntry)
	{
		DVCMAN_ENTRY_POINTS entryPoints = WINPR_C_ARRAY_INIT;

		entryPoints.iface.RegisterPlugin = dvcman_register_plugin;
		entryPoints.iface.GetPlugin = dvcman_get_plugin;
		entryPoints.iface.GetPluginData = dvcman_get_plugin_data;
		entryPoints.iface.GetRdpSettings = dvcman_get_rdp_settings;
		entryPoints.iface.GetRdpContext = dvcman_get_rdp_context;
		entryPoints.dvcman = (DVCMAN*)pChannelMgr;
		entryPoints.args = args;
		entryPoints.context = context;
		return pDVCPluginEntry(&entryPoints.iface);
	}

	return ERROR_INVALID_FUNCTION;
}

static void dvcman_channel_free(DVCMAN_CHANNEL* channel)
{
	if (!channel)
		return;

	if (channel->dvcman)
	{
		drdynvcPlugin* plugin = channel->dvcman->drdynvc;
		if (plugin)
		{
			rdpContext* context = plugin->rdpcontext;
			if (context)
			{
				ChannelTerminatedEventArgs e = WINPR_C_ARRAY_INIT;
				EventArgsInit(&e, "freerdp");
				e.name = channel->channel_name;
				e.pInterface = channel->pInterface;

				const int rc = PubSub_OnChannelTerminated(context->pubSub, context, &e);
				if (rc < 0)
					WLog_WARN(TAG, "PubSub_OnChannelTerminated(%s) failed", channel->channel_name);
			}
		}
	}

	if (channel->dvc_data)
		Stream_Release(channel->dvc_data);

	zgfx_context_free(channel->decompressor);
	DeleteCriticalSection(&(channel->lock));
	free(channel->channel_name);
	free(channel);
}

static void dvcman_channel_unref(DVCMAN_CHANNEL* channel)
{
	WINPR_ASSERT(channel);
	if (InterlockedDecrement(&channel->refCounter))
		return;

	DVCMAN* dvcman = channel->dvcman;
	if (dvcman)
		HashTable_Remove(dvcman->channelsById, &channel->channel_id);
}

static UINT dvcchannel_send_close(DVCMAN_CHANNEL* channel)
{
	WINPR_ASSERT(channel);
	DVCMAN* dvcman = channel->dvcman;
	drdynvcPlugin* drdynvc = dvcman->drdynvc;
	wStream* s = StreamPool_Take(dvcman->pool, 5);

	if (!s)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Write_UINT8(s, (CLOSE_REQUEST_PDU << 4) | 0x02);
	Stream_Write_UINT32(s, channel->channel_id);
	return drdynvc_send(drdynvc, s, &channel->stats);
}

static void check_open_close_receive(DVCMAN_CHANNEL* channel)
{
	WINPR_ASSERT(channel);

	IWTSVirtualChannelCallback* cb = channel->channel_callback;
	const char* name = channel->channel_name;
	const UINT32 id = channel->channel_id;

	WINPR_ASSERT(cb);
	if (!cb->OnOpen || !cb->OnClose || !cb->OnDataReceived)
		WLog_VRB(TAG, "{%s:%" PRIu32 "} OnOpen=%p, OnClose=%p, OnDataReceived=%p", name, id,
		         WINPR_FUNC_PTR_CAST(cb->OnOpen, const void*),
		         WINPR_FUNC_PTR_CAST(cb->OnClose, const void*),
		         WINPR_FUNC_PTR_CAST(cb->OnDataReceived, const void*));
}

static UINT dvcman_call_on_receive(DVCMAN_CHANNEL* channel, wStream* data)
{
	WINPR_ASSERT(channel);
	WINPR_ASSERT(data);

	channel->stats.packetsIn++;

	IWTSVirtualChannelCallback* cb = channel->channel_callback;
	WINPR_ASSERT(cb);

	check_open_close_receive(channel);
	WINPR_ASSERT(cb->OnDataReceived);
	return cb->OnDataReceived(cb, data);
}

static UINT dvcman_channel_close(DVCMAN_CHANNEL* channel, BOOL perRequest, BOOL fromHashTableFn)
{
	UINT error = CHANNEL_RC_OK;
	DrdynvcClientContext* context = nullptr;

	WINPR_ASSERT(channel);
	switch (channel->state)
	{
		case DVC_CHANNEL_INIT:
			break;
		case DVC_CHANNEL_RUNNING:
			if (channel->dvcman)
			{
				drdynvcPlugin* drdynvc = channel->dvcman->drdynvc;
				WINPR_ASSERT(drdynvc);
				context = drdynvc->context;
				if (perRequest)
					WLog_Print(drdynvc->log, WLOG_DEBUG, "sending close confirm for '%s'",
					           channel->channel_name);

				error = dvcchannel_send_close(channel);
				if (error != CHANNEL_RC_OK)
				{
					if (perRequest)
						WLog_Print(drdynvc->log, WLOG_DEBUG,
						           "error when sending closeRequest for '%s'",
						           channel->channel_name);
					else
						WLog_Print(drdynvc->log, WLOG_DEBUG,
						           "error when sending close confirm for '%s'",
						           channel->channel_name);
				}
				WLog_Print(drdynvc->log, WLOG_DEBUG, "listener %s destroyed channel %" PRIu32 "",
				           channel->channel_name, channel->channel_id);
			}

			channel->state = DVC_CHANNEL_CLOSED;

			{
				check_open_close_receive(channel);

				IWTSVirtualChannelCallback* cb = channel->channel_callback;
				channel->channel_callback = nullptr;
				if (cb)
					error = IFCALLRESULT(CHANNEL_RC_OK, cb->OnClose, cb);
			}

			if (channel->dvcman && channel->dvcman->drdynvc)
			{
				if (context)
				{
					IFCALLRET(context->OnChannelDisconnected, error, context, channel->channel_name,
					          channel->pInterface);
				}
			}

			if (!fromHashTableFn)
				dvcman_channel_unref(channel);
			break;
		case DVC_CHANNEL_CLOSED:
			break;
		default:
			break;
	}

	return error;
}

static DVCMAN_CHANNEL* dvcman_channel_new(drdynvcPlugin* drdynvc,
                                          IWTSVirtualChannelManager* pChannelMgr, UINT32 ChannelId,
                                          const char* ChannelName)
{
	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(pChannelMgr);
	DVCMAN_CHANNEL* channel = (DVCMAN_CHANNEL*)calloc(1, sizeof(DVCMAN_CHANNEL));

	if (!channel)
		return nullptr;

	channel->dvcman = (DVCMAN*)pChannelMgr;
	channel->channel_id = ChannelId;
	channel->refCounter = 1;
	channel->state = DVC_CHANNEL_INIT;
	channel->channel_name = _strdup(ChannelName);
	if (!channel->channel_name)
		goto fail;

	channel->decompressor = zgfx_context_new(FALSE);
	if (!channel->decompressor)
		goto fail;

	if (!InitializeCriticalSectionEx(&(channel->lock), 0, 0))
		goto fail;

	if (drdynvc)
	{
		rdpContext* context = drdynvc->rdpcontext;
		if (context)
		{
			ChannelInitializedEventArgs e = WINPR_C_ARRAY_INIT;
			EventArgsInit(&e, "freerdp");
			e.name = channel->channel_name;
			e.pInterface = channel->pInterface;

			const int rc = PubSub_OnChannelInitialized(context->pubSub, context, &e);
			if (rc < 0)
				WLog_WARN(TAG, "PubSub_OnChannelInitialized(%s) failed", channel->channel_name);
		}
	}

	return channel;
fail:
	dvcman_channel_free(channel);
	return nullptr;
}

static void dvcman_clear(drdynvcPlugin* drdynvc, IWTSVirtualChannelManager* pChannelMgr)
{
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;

	WINPR_ASSERT(dvcman);
	WINPR_UNUSED(drdynvc);

	HashTable_Clear(dvcman->channelsById);
	ArrayList_Clear(dvcman->plugins);
	ArrayList_Clear(dvcman->plugin_names);
	HashTable_Clear(dvcman->listeners);
}

static void dvcman_free(drdynvcPlugin* drdynvc, IWTSVirtualChannelManager* pChannelMgr)
{
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;

	WINPR_ASSERT(dvcman);
	WINPR_UNUSED(drdynvc);

	HashTable_Free(dvcman->channelsById);
	ArrayList_Free(dvcman->plugins);
	ArrayList_Free(dvcman->plugin_names);
	HashTable_Free(dvcman->listeners);

	StreamPool_Free(dvcman->pool);
	free(dvcman);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_init(drdynvcPlugin* drdynvc, IWTSVirtualChannelManager* pChannelMgr)
{
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(dvcman);
	ArrayList_Lock(dvcman->plugins);
	for (size_t i = 0; i < ArrayList_Count(dvcman->plugins); i++)
	{
		IWTSPlugin* pPlugin = ArrayList_GetItem(dvcman->plugins, i);

		error = IFCALLRESULT(CHANNEL_RC_OK, pPlugin->Initialize, pPlugin, pChannelMgr);
		if (error != CHANNEL_RC_OK)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "Initialize failed with error %" PRIu32 "!",
			           error);
			goto fail;
		}
	}

fail:
	ArrayList_Unlock(dvcman->plugins);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_write_channel(IWTSVirtualChannel* pChannel, ULONG cbSize, const BYTE* pBuffer,
                                 void* pReserved)
{
	BOOL close = FALSE;
	UINT status = 0;
	DVCMAN_CHANNEL* channel = (DVCMAN_CHANNEL*)pChannel;

	WINPR_UNUSED(pReserved);
	if (!channel || !channel->dvcman)
		return CHANNEL_RC_BAD_CHANNEL;
	drdynvcPlugin* drdynvc = channel->dvcman->drdynvc;
	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	EnterCriticalSection(&(channel->lock));
	EnterCriticalSection(&drdynvc->udpLock);
	status = drdynvc_write_data(drdynvc, channel, pBuffer, cbSize, &close, &channel->stats);
	LeaveCriticalSection(&drdynvc->udpLock);
	LeaveCriticalSection(&(channel->lock));
	/* Close delayed, it removes the channel struct */
	if (close)
		dvcman_channel_close(channel, FALSE, FALSE);

	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_close_channel_iface(IWTSVirtualChannel* pChannel)
{
	DVCMAN_CHANNEL* channel = (DVCMAN_CHANNEL*)pChannel;

	if (!channel)
		return CHANNEL_RC_BAD_CHANNEL;

	WLog_DBG(TAG, "close_channel_iface: id=%" PRIu32 "", channel->channel_id);
	return dvcman_channel_close(channel, FALSE, FALSE);
}

struct stats_collector_argument
{
	DrdynvcClientChannelStat* stats;
	size_t count;
	size_t used;
};

static BOOL stats_collector(WINPR_ATTR_UNUSED const void* key, void* value, void* arg)
{
	struct stats_collector_argument* args = arg;
	WINPR_ASSERT(args);

	DVCMAN_CHANNEL* channel = value;
	WINPR_ASSERT(channel);
	if (args->used >= args->count)
		return FALSE;

	DrdynvcClientChannelStat* stat = &args->stats[args->used++];

	if (channel->channel_name)
	{
		const size_t len = strnlen(channel->channel_name, ARRAYSIZE(stat->channelName) - 1);
		strncpy(stat->channelName, channel->channel_name, len);
	}
	else
		memset(stat->channelName, 0, sizeof(stat->channelName));
	stat->channelId = channel->channel_id;
	stat->bytesIn = channel->stats.bytesIn;
	stat->bytesOut = channel->stats.bytesOut;
	stat->fragmentsIn = channel->stats.fragmentsIn;
	stat->fragmentsOut = channel->stats.fragmentsOut;
	stat->packetsIn = channel->stats.packetsIn;
	stat->packetsOut = channel->stats.packetsOut;
	return TRUE;
}

WINPR_ATTR_MALLOC(free, 1)
static DrdynvcClientChannelStat* drdynvc_get_channel_stats(DrdynvcClientContext* context,
                                                           size_t* pCount)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(pCount);

	drdynvcPlugin* drdynvc = (drdynvcPlugin*)context->handle;
	WINPR_ASSERT(drdynvc);

	DVCMAN* dvcman = (DVCMAN*)drdynvc->channel_mgr;
	*pCount = 0;
	if (!dvcman)
		return nullptr;

	struct stats_collector_argument args = { .stats = nullptr, .count = 0 };

	HashTable_Lock(dvcman->channelsById);
	const size_t count = HashTable_Count(dvcman->channelsById);
	if (count > 0)
	{
		args.stats = calloc(count, sizeof(DrdynvcClientChannelStat));
		if (args.stats)
		{
			args.count = count;
			const BOOL rc = HashTable_Foreach(dvcman->channelsById, stats_collector, &args);
			if (!rc)
			{
				HashTable_Unlock(dvcman->channelsById);
				free(args.stats);
				return nullptr;
			}
		}
	}
	HashTable_Unlock(dvcman->channelsById);
	*pCount = args.used;
	return args.stats;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static DVCMAN_CHANNEL* dvcman_create_channel(drdynvcPlugin* drdynvc,
                                             IWTSVirtualChannelManager* pChannelMgr,
                                             UINT32 ChannelId, const char* ChannelName, UINT* res)
{
	BOOL bAccept = 0;
	DVCMAN_CHANNEL* channel = nullptr;
	DrdynvcClientContext* context = nullptr;
	DVCMAN* dvcman = (DVCMAN*)pChannelMgr;
	DVCMAN_LISTENER* listener = nullptr;
	IWTSVirtualChannelCallback* pCallback = nullptr;

	WINPR_ASSERT(dvcman);
	WINPR_ASSERT(res);

	HashTable_Lock(dvcman->listeners);
	listener = (DVCMAN_LISTENER*)HashTable_GetItemValue(dvcman->listeners, ChannelName);
	if (!listener)
	{
		*res = ERROR_NOT_FOUND;
		goto out;
	}

	channel = dvcman_get_channel_by_id(pChannelMgr, ChannelId, FALSE);
	if (channel)
	{
		switch (channel->state)
		{
			case DVC_CHANNEL_RUNNING:
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "Protocol error: Duplicated ChannelId %" PRIu32 " (%s)!", ChannelId,
				           ChannelName);
				*res = CHANNEL_RC_ALREADY_OPEN;
				goto out;

			case DVC_CHANNEL_CLOSED:
			case DVC_CHANNEL_INIT:
			default:
			{
				WLog_Print(drdynvc->log, WLOG_ERROR, "not expecting a createChannel from state %s",
				           channel_state2str(channel->state));
				*res = CHANNEL_RC_INITIALIZATION_ERROR;
				goto out;
			}
		}
	}
	else
	{
		if (!(channel = dvcman_channel_new(drdynvc, pChannelMgr, ChannelId, ChannelName)))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "dvcman_channel_new failed!");
			*res = CHANNEL_RC_NO_MEMORY;
			goto out;
		}
	}

	if (!HashTable_Insert(dvcman->channelsById, &channel->channel_id, channel))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "unable to register channel in our channel list");
		*res = ERROR_INTERNAL_ERROR;
		dvcman_channel_free(channel);
		channel = nullptr;
		goto out;
	}

	channel->iface.Write = dvcman_write_channel;
	channel->iface.Close = dvcman_close_channel_iface;
	bAccept = TRUE;

	*res = listener->listener_callback->OnNewChannelConnection(
	    listener->listener_callback, &channel->iface, nullptr, &bAccept, &pCallback);

	if (*res != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR,
		           "OnNewChannelConnection failed with error %" PRIu32 "!", *res);
		*res = ERROR_INTERNAL_ERROR;
		dvcman_channel_unref(channel);
		goto out;
	}

	if (!bAccept)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "OnNewChannelConnection returned with bAccept FALSE!");
		*res = ERROR_INTERNAL_ERROR;
		dvcman_channel_unref(channel);
		channel = nullptr;
		goto out;
	}

	WLog_Print(drdynvc->log, WLOG_DEBUG, "listener %s created new channel %" PRIu32 "",
	           listener->channel_name, channel->channel_id);
	channel->state = DVC_CHANNEL_RUNNING;
	channel->channel_callback = pCallback;
	channel->pInterface = listener->iface.pInterface;
	context = dvcman->drdynvc->context;

	IFCALLRET(context->OnChannelConnected, *res, context, ChannelName, listener->iface.pInterface);
	if (*res != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR,
		           "context.OnChannelConnected failed with error %" PRIu32 "", *res);
	}

out:
	HashTable_Unlock(dvcman->listeners);

	return channel;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_open_channel(drdynvcPlugin* drdynvc, DVCMAN_CHANNEL* channel)
{
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(channel);
	if (channel->state == DVC_CHANNEL_RUNNING)
	{
		IWTSVirtualChannelCallback* pCallback = channel->channel_callback;

		if (pCallback->OnOpen)
		{
			check_open_close_receive(channel);
			error = pCallback->OnOpen(pCallback);
			if (error)
			{
				WLog_Print(drdynvc->log, WLOG_ERROR, "OnOpen failed with error %" PRIu32 "!",
				           error);
				goto out;
			}
		}

		WLog_Print(drdynvc->log, WLOG_DEBUG, "open_channel: ChannelId %" PRIu32 "",
		           channel->channel_id);
	}

out:
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_receive_channel_data_first(DVCMAN_CHANNEL* channel, UINT32 length)
{
	WINPR_ASSERT(channel);
	WINPR_ASSERT(channel->dvcman);
	if (channel->dvc_data)
		Stream_Release(channel->dvc_data);

	channel->dvc_data = StreamPool_Take(channel->dvcman->pool, length);

	if (!channel->dvc_data)
	{
		drdynvcPlugin* drdynvc = channel->dvcman->drdynvc;
		WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	channel->dvc_data_length = length;
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT dvcman_receive_channel_data(DVCMAN_CHANNEL* channel, wStream* data,
                                        WINPR_ATTR_UNUSED UINT32 ThreadingFlags)
{
	UINT status = CHANNEL_RC_OK;
	size_t dataSize = Stream_GetRemainingLength(data);

	WINPR_ASSERT(channel);
	WINPR_ASSERT(channel->dvcman);

	channel->stats.bytesIn += Stream_Length(data);
	if (channel->dvc_data)
	{
		drdynvcPlugin* drdynvc = channel->dvcman->drdynvc;

		/* Fragmented data */
		if (Stream_GetPosition(channel->dvc_data) + dataSize > channel->dvc_data_length)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "data exceeding declared length!");
			Stream_Release(channel->dvc_data);
			channel->dvc_data = nullptr;
			status = ERROR_INVALID_DATA;
			goto out;
		}

		Stream_Copy(data, channel->dvc_data, dataSize);
		channel->stats.fragmentsIn++;

		if (Stream_GetPosition(channel->dvc_data) >= channel->dvc_data_length)
		{
			Stream_SealLength(channel->dvc_data);
			Stream_ResetPosition(channel->dvc_data);

			status = dvcman_call_on_receive(channel, channel->dvc_data);
			Stream_Release(channel->dvc_data);
			channel->dvc_data = nullptr;
		}
	}
	else
		status = dvcman_call_on_receive(channel, data);

out:
	return status;
}

static UINT8 drdynvc_write_variable_uint(wStream* s, UINT32 val)
{
	UINT8 cb = 0;

	if (val <= 0xFF)
	{
		cb = 0;
		Stream_Write_UINT8(s, (UINT8)val);
	}
	else if (val <= 0xFFFF)
	{
		cb = 1;
		Stream_Write_UINT16(s, (UINT16)val);
	}
	else
	{
		cb = 2;
		Stream_Write_UINT32(s, val);
	}

	return cb;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_send_internal(drdynvcPlugin* drdynvc, wStream* s, DVCMAN_CHANNEL_STATS* stats,
                                  BOOL ignoreNotConnected)
{
	UINT status = 0;

	if (!drdynvc)
		status = CHANNEL_RC_BAD_CHANNEL_HANDLE;
	else
	{
		const size_t len = Stream_GetPosition(s);

		if (stats)
			stats->bytesOut += len;

		WINPR_ASSERT(drdynvc->channelEntryPoints.pVirtualChannelWriteEx);
		status = drdynvc->channelEntryPoints.pVirtualChannelWriteEx(
		    drdynvc->InitHandle, drdynvc->OpenHandle, Stream_Buffer(s), (UINT32)len, s);
	}

	switch (status)
	{
		case CHANNEL_RC_OK:
			return CHANNEL_RC_OK;

		case CHANNEL_RC_NOT_CONNECTED:
			Stream_Release(s);
			return ignoreNotConnected ? CHANNEL_RC_OK : status;

		case CHANNEL_RC_BAD_CHANNEL_HANDLE:
			Stream_Release(s);
			WLog_ERR(TAG, "VirtualChannelWriteEx failed with CHANNEL_RC_BAD_CHANNEL_HANDLE");
			return status;

		default:
			Stream_Release(s);
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "VirtualChannelWriteEx failed with %s [%08" PRIX32 "]",
			           WTSErrorToString(status), status);
			return status;
	}
}

static UINT drdynvc_send(drdynvcPlugin* drdynvc, wStream* s, DVCMAN_CHANNEL_STATS* stats)
{
	return drdynvc_send_internal(drdynvc, s, stats, TRUE);
}

static UINT drdynvc_send_strict(drdynvcPlugin* drdynvc, wStream* s, DVCMAN_CHANNEL_STATS* stats)
{
	return drdynvc_send_internal(drdynvc, s, stats, FALSE);
}

static UINT drdynvc_send_channel_data(drdynvcPlugin* drdynvc, DVCMAN_CHANNEL* channel,
                                      UINT32 tunnelType, UINT64 routeGeneration, wStream* s,
                                      DVCMAN_CHANNEL_STATS* stats)
{
	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(channel);
	WINPR_ASSERT(s);

	EnterCriticalSection(&drdynvc->udpLock);
	if (drdynvc->udpGeneration != routeGeneration)
	{
		LeaveCriticalSection(&drdynvc->udpLock);
		Stream_Release(s);
		return ERROR_INVALID_STATE;
	}
	if (tunnelType == 0)
	{
		const UINT status = drdynvc_send(drdynvc, s, stats);
		LeaveCriticalSection(&drdynvc->udpLock);
		return status;
	}

	const UINT32 tunnelMask = drdynvc_udp_tunnel_mask(tunnelType);
	if (!drdynvc->softSyncActive || !drdynvc->udpSend ||
	    ((drdynvc->udpNegotiatedMask & tunnelMask) == 0) ||
	    ((drdynvc->udpTunnelMask & tunnelMask) == 0))
	{
		LeaveCriticalSection(&drdynvc->udpLock);
		Stream_Release(s);
		return ERROR_INVALID_STATE;
	}

	const size_t length = Stream_GetPosition(s);
	if (stats)
		stats->bytesOut += length;

	const UINT status = drdynvc->udpSend(drdynvc->udpUserData, tunnelType, Stream_Buffer(s),
	                                     WINPR_ASSERTING_INT_CAST(UINT32, length));
	LeaveCriticalSection(&drdynvc->udpLock);
	Stream_Release(s);

	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR,
		           "UDP multitransport write failed for channel %" PRIu32 " on tunnel %" PRIu32
		           " with %s [%08" PRIX32 "]",
		           channel->channel_id, tunnelType, WTSErrorToString(status), status);
	}
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_write_data(drdynvcPlugin* drdynvc, DVCMAN_CHANNEL* channel, const BYTE* data,
                               UINT32 dataSize, BOOL* close, DVCMAN_CHANNEL_STATS* stats)
{
	size_t pos = 0;
	UINT8 cbChId = 0;
	UINT8 cbLen = 0;
	UINT status = CHANNEL_RC_BAD_INIT_HANDLE;
	DVCMAN* dvcman = nullptr;

	if (!drdynvc || !channel)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;
	const UINT32 ChannelId = channel->channel_id;
	EnterCriticalSection(&drdynvc->udpLock);
	const UINT32 tunnelType = channel->udpTunnelType;
	const UINT64 routeGeneration = drdynvc->udpGeneration;
	LeaveCriticalSection(&drdynvc->udpLock);

	dvcman = (DVCMAN*)drdynvc->channel_mgr;
	WINPR_ASSERT(dvcman);

	WLog_Print(drdynvc->log, WLOG_TRACE, "write_data: ChannelId=%" PRIu32 " size=%" PRIu32 "",
	           ChannelId, dataSize);
	wStream* data_out = StreamPool_Take(dvcman->pool, CHANNEL_CHUNK_LENGTH);

	if (!data_out)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	if (!Stream_SetPosition(data_out, 1))
	{
		Stream_Release(data_out);
		return ERROR_INVALID_DATA;
	}
	cbChId = drdynvc_write_variable_uint(data_out, ChannelId);
	pos = Stream_GetPosition(data_out);
	WINPR_ASSERT(pos <= CHANNEL_CHUNK_LENGTH);

	/* MS-RDPEDYC 3.1.5 forbids fragmented DVC data on UDP-L. Do not silently
	 * fall back to TCP or another tunnel after Soft-Sync because that would lose
	 * the per-channel ordering guarantee. A loss-tolerant endpoint must submit a
	 * message that fits in one DYNVC_DATA PDU. */
	if ((tunnelType == TUNNELTYPE_UDPFECL) && (dataSize > (CHANNEL_CHUNK_LENGTH - pos)))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR,
		           "Refusing fragmented UDP-L DVC message for channel %" PRIu32 " (size=%" PRIu32
		           ", max=%" PRIuz ")",
		           ChannelId, dataSize, CHANNEL_CHUNK_LENGTH - pos);
		Stream_Release(data_out);
		return ERROR_INVALID_DATA;
	}

	if (dataSize == 0)
	{
		/* TODO: shall treat that case with write(0) that do a close */
		*close = TRUE;
		Stream_Release(data_out);
	}
	else if (dataSize <= CHANNEL_CHUNK_LENGTH - pos)
	{
		Stream_ResetPosition(data_out);
		Stream_Write_UINT8(data_out, (DATA_PDU << 4) | cbChId);
		if (!Stream_SetPosition(data_out, pos))
		{
			Stream_Release(data_out);
			return ERROR_INVALID_DATA;
		}
		Stream_Write(data_out, data, dataSize);
		stats->packetsOut++;
		status = drdynvc_send_channel_data(drdynvc, channel, tunnelType, routeGeneration, data_out,
		                                   stats);
	}
	else
	{
		/* Fragment the data */
		cbLen = drdynvc_write_variable_uint(data_out, dataSize);
		pos = Stream_GetPosition(data_out);
		Stream_ResetPosition(data_out);

		const INT32 pdu = (DATA_FIRST_PDU << 4) | cbChId | (cbLen << 2);
		Stream_Write_UINT8(data_out, WINPR_ASSERTING_INT_CAST(UINT8, pdu));
		if (!Stream_SetPosition(data_out, pos))
		{
			Stream_Release(data_out);
			return ERROR_INVALID_DATA;
		}

		{
			WINPR_ASSERT(pos <= CHANNEL_CHUNK_LENGTH);
			const uint32_t chunkLength =
			    CHANNEL_CHUNK_LENGTH - WINPR_ASSERTING_INT_CAST(uint32_t, pos);
			Stream_Write(data_out, data, chunkLength);

			data += chunkLength;
			dataSize -= chunkLength;
		}
		if (dataSize > 0)
			stats->fragmentsOut++;

		status = drdynvc_send_channel_data(drdynvc, channel, tunnelType, routeGeneration, data_out,
		                                   stats);

		while (status == CHANNEL_RC_OK && dataSize > 0)
		{
			data_out = StreamPool_Take(dvcman->pool, CHANNEL_CHUNK_LENGTH);

			if (!data_out)
			{
				WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
				return CHANNEL_RC_NO_MEMORY;
			}

			if (!Stream_SetPosition(data_out, 1))
			{
				Stream_Release(data_out);
				return ERROR_INVALID_DATA;
			}

			cbChId = drdynvc_write_variable_uint(data_out, ChannelId);
			pos = Stream_GetPosition(data_out);
			Stream_ResetPosition(data_out);
			Stream_Write_UINT8(data_out, (DATA_PDU << 4) | cbChId);
			if (!Stream_SetPosition(data_out, pos))
			{
				Stream_Release(data_out);
				return ERROR_INVALID_DATA;
			}

			uint32_t chunkLength = dataSize;

			WINPR_ASSERT(pos <= CHANNEL_CHUNK_LENGTH);
			const uint32_t clen = CHANNEL_CHUNK_LENGTH - WINPR_ASSERTING_INT_CAST(uint32_t, pos);
			if (chunkLength > clen)
			{
				stats->fragmentsOut++;
				chunkLength = clen;
			}
			else
				stats->packetsOut++;

			Stream_Write(data_out, data, chunkLength);
			data += chunkLength;
			dataSize -= chunkLength;

			status = drdynvc_send_channel_data(drdynvc, channel, tunnelType, routeGeneration,
			                                   data_out, stats);
		}
	}

	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "DVC channel data send failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(status), status);
		return status;
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_send_capability_response(drdynvcPlugin* drdynvc)
{
	UINT status = 0;
	wStream* s = nullptr;
	DVCMAN* dvcman = nullptr;

	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	dvcman = (DVCMAN*)drdynvc->channel_mgr;
	WINPR_ASSERT(dvcman);

	WLog_Print(drdynvc->log, WLOG_TRACE, "capability_response");
	s = StreamPool_Take(dvcman->pool, 4);

	if (!s)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Write_UINT16(s, 0x0050); /* Cmd+Sp+cbChId+Pad. Note: MSTSC sends 0x005c */
	Stream_Write_UINT16(s, drdynvc->version);
	status = drdynvc_send(drdynvc, s, nullptr);

	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "VirtualChannelWriteEx failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(status), status);
	}

	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_process_capability_request(drdynvcPlugin* drdynvc, int Sp, int cbChId,
                                               wStream* s)
{
	UINT status = 0;

	if (!drdynvc)
		return CHANNEL_RC_BAD_INIT_HANDLE;

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 3))
		return ERROR_INVALID_DATA;

	WLog_Print(drdynvc->log, WLOG_TRACE, "capability_request Sp=%d cbChId=%d", Sp, cbChId);
	Stream_Seek(s, 1); /* pad */
	Stream_Read_UINT16(s, drdynvc->version);

	/* RDP8 servers offer version 3, though Microsoft forgot to document it
	 * in their early documents.  It behaves the same as version 2.
	 */
	if ((drdynvc->version == 2) || (drdynvc->version == 3))
	{
		if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
			return ERROR_INVALID_DATA;

		Stream_Read_UINT16(s, drdynvc->PriorityCharge0);
		Stream_Read_UINT16(s, drdynvc->PriorityCharge1);
		Stream_Read_UINT16(s, drdynvc->PriorityCharge2);
		Stream_Read_UINT16(s, drdynvc->PriorityCharge3);
	}

	status = drdynvc_send_capability_response(drdynvc);
	drdynvc->state = DRDYNVC_STATE_READY;
	return status;
}

static UINT32 drdynvc_cblen_to_bytes(int cbLen)
{
	switch (cbLen)
	{
		case 0:
			return 1;

		case 1:
			return 2;

		default:
			return 4;
	}
}

static UINT32 drdynvc_read_variable_uint(wStream* s, int cbLen)
{
	UINT32 val = 0;

	switch (cbLen)
	{
		case 0:
			Stream_Read_UINT8(s, val);
			break;

		case 1:
			Stream_Read_UINT16(s, val);
			break;

		default:
			Stream_Read_UINT32(s, val);
			break;
	}

	return val;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_process_create_request(drdynvcPlugin* drdynvc, UINT8 Sp, UINT8 cbChId,
                                           wStream* s)
{
	UINT status = 0;
	wStream* data_out = nullptr;
	UINT channel_status = 0;
	DVCMAN* dvcman = nullptr;
	DVCMAN_CHANNEL* channel = nullptr;
	INT32 retStatus = 0;

	WINPR_UNUSED(Sp);
	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	dvcman = (DVCMAN*)drdynvc->channel_mgr;
	WINPR_ASSERT(dvcman);

	if (drdynvc->state == DRDYNVC_STATE_CAPABILITIES)
	{
		/**
		 * For some reason the server does not always send the
		 * capabilities pdu as it should. When this happens,
		 * send a capabilities response.
		 */
		drdynvc->version = 3;

		if ((status = drdynvc_send_capability_response(drdynvc)))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "drdynvc_send_capability_response failed!");
			return status;
		}

		drdynvc->state = DRDYNVC_STATE_READY;
	}

	if (!Stream_CheckAndLogRequiredLength(TAG, s, drdynvc_cblen_to_bytes(cbChId)))
		return ERROR_INVALID_DATA;

	const UINT32 ChannelId = drdynvc_read_variable_uint(s, cbChId);
	const size_t pos = Stream_GetPosition(s);
	const char* name = Stream_ConstPointer(s);
	const size_t length = Stream_GetRemainingLength(s);

	if (strnlen(name, length) >= length)
		return ERROR_INVALID_DATA;

	WLog_Print(drdynvc->log, WLOG_DEBUG,
	           "process_create_request: ChannelId=%" PRIu32 " ChannelName=%s", ChannelId, name);

	data_out = StreamPool_Take(dvcman->pool, pos + 4);
	if (!data_out)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Write_UINT8(data_out, (CREATE_REQUEST_PDU << 4) | cbChId);
	if (!Stream_SetPosition(s, 1))
		return ERROR_INVALID_DATA;
	Stream_Copy(s, data_out, pos - 1);

	channel =
	    dvcman_create_channel(drdynvc, drdynvc->channel_mgr, ChannelId, name, &channel_status);
	switch (channel_status)
	{
		case CHANNEL_RC_OK:
			WLog_Print(drdynvc->log, WLOG_DEBUG, "channel created");
			retStatus = 0;
			break;
		case CHANNEL_RC_NO_MEMORY:
			WLog_Print(drdynvc->log, WLOG_DEBUG, "not enough memory for channel creation");
			retStatus = STATUS_NO_MEMORY;
			break;
		case ERROR_NOT_FOUND:
			WLog_Print(drdynvc->log, WLOG_DEBUG, "no listener for '%s'", name);
			retStatus = STATUS_NOT_FOUND; /* same code used by mstsc, STATUS_UNSUCCESSFUL */
			break;
		default:
			WLog_Print(drdynvc->log, WLOG_DEBUG, "channel creation error");
			retStatus = STATUS_UNSUCCESSFUL; /* same code used by mstsc, STATUS_UNSUCCESSFUL */
			break;
	}
	Stream_Write_INT32(data_out, retStatus);

	status = drdynvc_send(drdynvc, data_out, nullptr);
	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "VirtualChannelWriteEx failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(status), status);
		dvcman_channel_unref(channel);
		return status;
	}

	if (channel_status == CHANNEL_RC_OK)
	{
		if ((status = dvcman_open_channel(drdynvc, channel)))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "dvcman_open_channel failed with error %" PRIu32 "!", status);
			return status;
		}
	}

	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_process_data_first(drdynvcPlugin* drdynvc, int Sp, int cbChId, wStream* s,
                                       BOOL compressed, UINT32 ThreadingFlags)
{
	WINPR_ASSERT(drdynvc);
	if (!Stream_CheckAndLogRequiredLength(
	        TAG, s, drdynvc_cblen_to_bytes(cbChId) + drdynvc_cblen_to_bytes(Sp)))
		return ERROR_INVALID_DATA;

	UINT32 ChannelId = drdynvc_read_variable_uint(s, cbChId);
	UINT32 Length = drdynvc_read_variable_uint(s, Sp);
	WLog_Print(drdynvc->log, WLOG_TRACE,
	           "process_data_first: Sp=%d cbChId=%d, ChannelId=%" PRIu32 " Length=%" PRIu32 "", Sp,
	           cbChId, ChannelId, Length);

	DVCMAN_CHANNEL* channel = dvcman_get_channel_by_id(drdynvc->channel_mgr, ChannelId, TRUE);
	if (!channel)
	{
		/**
		 * Windows Server 2012 R2 can send some messages over
		 * Microsoft::Windows::RDS::Geometry::v08.01 even if the dynamic virtual channel wasn't
		 * registered on our side. Ignoring it works.
		 */
		WLog_Print(drdynvc->log, WLOG_ERROR, "ChannelId %" PRIu32 " not found!", ChannelId);
		return CHANNEL_RC_OK;
	}

	UINT status = CHANNEL_RC_OK;
	BOOL shouldFree = FALSE;
	if (channel->state != DVC_CHANNEL_RUNNING)
		goto out;

	if (compressed)
	{
		BYTE* data = nullptr;
		UINT32 dataSize = 0;
		if (zgfx_decompress(channel->decompressor, Stream_Pointer(s),
		                    WINPR_ASSERTING_INT_CAST(UINT32, Stream_GetRemainingLength(s)), &data,
		                    &dataSize, 0) < 0)
		{
			status = ERROR_INVALID_DATA;
			WLog_Print(drdynvc->log, WLOG_ERROR, "error de-compressing first packet");
			goto out;
		}

		s = Stream_New(data, dataSize);
		if (!s)
		{
			status = CHANNEL_RC_NO_MEMORY;
			WLog_Print(drdynvc->log, WLOG_ERROR, "error allocating new Stream(len=%" PRIu32 ")",
			           dataSize);
			free(data);
			goto out;
		}
		shouldFree = TRUE;
	}

	status = dvcman_receive_channel_data_first(channel, Length);

	if (status == CHANNEL_RC_OK)
		status = dvcman_receive_channel_data(channel, s, ThreadingFlags);

	if (status != CHANNEL_RC_OK)
		status = dvcman_channel_close(channel, FALSE, FALSE);

out:
	if (shouldFree)
		Stream_Free(s, TRUE);
	dvcman_channel_unref(channel);
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_process_data(drdynvcPlugin* drdynvc, int Sp, int cbChId, wStream* s,
                                 BOOL compressed, UINT32 ThreadingFlags)
{
	WINPR_ASSERT(drdynvc);
	if (!Stream_CheckAndLogRequiredLength(TAG, s, drdynvc_cblen_to_bytes(cbChId)))
		return ERROR_INVALID_DATA;

	UINT32 ChannelId = drdynvc_read_variable_uint(s, cbChId);
	WLog_Print(drdynvc->log, WLOG_TRACE, "process_data: Sp=%d cbChId=%d, ChannelId=%" PRIu32 "", Sp,
	           cbChId, ChannelId);

	DVCMAN_CHANNEL* channel = dvcman_get_channel_by_id(drdynvc->channel_mgr, ChannelId, TRUE);
	if (!channel)
	{
		/**
		 * Windows Server 2012 R2 can send some messages over
		 * Microsoft::Windows::RDS::Geometry::v08.01 even if the dynamic virtual channel wasn't
		 * registered on our side. Ignoring it works.
		 */
		WLog_Print(drdynvc->log, WLOG_ERROR, "ChannelId %" PRIu32 " not found!", ChannelId);
		return CHANNEL_RC_OK;
	}

	BOOL shouldFree = FALSE;
	UINT status = CHANNEL_RC_OK;
	if (channel->state != DVC_CHANNEL_RUNNING)
		goto out;

	if (compressed)
	{
		BYTE* data = nullptr;
		UINT32 dataSize = 0;

		if (zgfx_decompress(channel->decompressor, Stream_Pointer(s),
		                    WINPR_ASSERTING_INT_CAST(UINT32, Stream_GetRemainingLength(s)), &data,
		                    &dataSize, 0) < 0)
		{
			status = ERROR_INVALID_DATA;
			WLog_Print(drdynvc->log, WLOG_ERROR, "error de-compressing data packet");
			goto out;
		}

		s = Stream_New(data, dataSize);
		if (!s)
		{
			status = CHANNEL_RC_NO_MEMORY;
			WLog_Print(drdynvc->log, WLOG_ERROR, "error allocating new Stream(len=%" PRIu32 ")",
			           dataSize);
			free(data);
			goto out;
		}
		shouldFree = TRUE;
	}

	status = dvcman_receive_channel_data(channel, s, ThreadingFlags);
	if (status != CHANNEL_RC_OK)
		status = dvcman_channel_close(channel, FALSE, FALSE);

out:
	if (shouldFree)
		Stream_Free(s, TRUE);
	dvcman_channel_unref(channel);
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_process_close_request(drdynvcPlugin* drdynvc, int Sp, int cbChId, wStream* s)
{
	UINT32 ChannelId = 0;
	DVCMAN_CHANNEL* channel = nullptr;

	WINPR_ASSERT(drdynvc);
	if (!Stream_CheckAndLogRequiredLength(TAG, s, drdynvc_cblen_to_bytes(cbChId)))
		return ERROR_INVALID_DATA;

	ChannelId = drdynvc_read_variable_uint(s, cbChId);
	WLog_Print(drdynvc->log, WLOG_DEBUG,
	           "process_close_request: Sp=%d cbChId=%d, ChannelId=%" PRIu32 "", Sp, cbChId,
	           ChannelId);

	channel = dvcman_get_channel_by_id(drdynvc->channel_mgr, ChannelId, TRUE);
	if (!channel)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "dvcman_close_request channel %" PRIu32 " not present",
		           ChannelId);
		return CHANNEL_RC_OK;
	}

	dvcman_channel_close(channel, TRUE, FALSE);
	dvcman_channel_unref(channel);
	return CHANNEL_RC_OK;
}

typedef struct
{
	UINT32 channelId;
	UINT32 tunnelType;
	DVCMAN_CHANNEL* channel;
} DRDYNVC_SOFT_SYNC_ROUTE;

static int drdynvc_soft_sync_route_compare(const void* lhs, const void* rhs)
{
	const DRDYNVC_SOFT_SYNC_ROUTE* left = (const DRDYNVC_SOFT_SYNC_ROUTE*)lhs;
	const DRDYNVC_SOFT_SYNC_ROUTE* right = (const DRDYNVC_SOFT_SYNC_ROUTE*)rhs;
	if (left->channelId < right->channelId)
		return -1;
	if (left->channelId > right->channelId)
		return 1;
	return 0;
}

static void drdynvc_soft_sync_routes_free(DRDYNVC_SOFT_SYNC_ROUTE* routes, size_t count)
{
	if (!routes)
		return;

	for (size_t x = 0; x < count; x++)
	{
		if (routes[x].channel)
			dvcman_channel_unref(routes[x].channel);
	}
	free(routes);
}

static UINT drdynvc_process_soft_sync_request(drdynvcPlugin* drdynvc, int Sp, int cbChId,
                                              wStream* s)
{
	UINT status = ERROR_INVALID_DATA;
	UINT8 pad = 0;
	UINT16 flags = 0;
	UINT16 numTunnels = 0;
	UINT32 length = 0;
	UINT32 requestedMask = 0;
	UINT32 tunnelTypes[2] = { 0 };
	size_t routeCount = 0;
	size_t routeCapacity = 0;
	DRDYNVC_SOFT_SYNC_ROUTE* routes = nullptr;

	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(s);
	if ((Sp != 0) || (cbChId != 0) || (drdynvc->state != DRDYNVC_STATE_READY) ||
	    !drdynvc->channel_mgr || !drdynvc->rdpcontext || !drdynvc->rdpcontext->settings ||
	    ((freerdp_settings_get_uint32(drdynvc->rdpcontext->settings, FreeRDP_MultitransportFlags) &
	      SOFTSYNC_TCP_TO_UDP) == 0) ||
	    !Stream_CheckAndLogRequiredLength(TAG, s, 9))
		goto fail;

	Stream_Read_UINT8(s, pad);
	const size_t encodedLength = Stream_GetRemainingLength(s);
	Stream_Read_UINT32(s, length);
	Stream_Read_UINT16(s, flags);
	Stream_Read_UINT16(s, numTunnels);

	if ((pad != 0) || (length < 8) || ((size_t)length != encodedLength) ||
	    ((flags & SOFT_SYNC_TCP_FLUSHED) == 0) ||
	    ((flags & ~(SOFT_SYNC_TCP_FLUSHED | SOFT_SYNC_CHANNEL_LIST_PRESENT)) != 0))
		goto fail;

	const BOOL listsPresent = (flags & SOFT_SYNC_CHANNEL_LIST_PRESENT) != 0;
	if ((!listsPresent && (numTunnels != 0)) ||
	    (listsPresent && ((numTunnels == 0) || (numTunnels > ARRAYSIZE(tunnelTypes)))))
		goto fail;

	routeCapacity = Stream_GetRemainingLength(s) / sizeof(UINT32);
	if (routeCapacity > (SIZE_MAX / sizeof(*routes)))
	{
		status = CHANNEL_RC_NO_MEMORY;
		goto fail;
	}
	if (routeCapacity > 0)
	{
		routes = (DRDYNVC_SOFT_SYNC_ROUTE*)calloc(routeCapacity, sizeof(*routes));
		if (!routes)
		{
			status = CHANNEL_RC_NO_MEMORY;
			goto fail;
		}
	}

	for (UINT16 x = 0; x < numTunnels; x++)
	{
		UINT16 channelCount = 0;
		UINT32 tunnelType = 0;
		if (!Stream_CheckAndLogRequiredLength(TAG, s, 6))
			goto fail;

		Stream_Read_UINT32(s, tunnelType);
		Stream_Read_UINT16(s, channelCount);
		const UINT32 tunnelMask = drdynvc_udp_tunnel_mask(tunnelType);
		if ((tunnelMask == 0) || (channelCount == 0) || ((requestedMask & tunnelMask) != 0) ||
		    ((size_t)channelCount > (Stream_GetRemainingLength(s) / sizeof(UINT32))) ||
		    ((size_t)channelCount > (routeCapacity - routeCount)))
			goto fail;

		tunnelTypes[x] = tunnelType;
		requestedMask |= tunnelMask;
		for (UINT16 y = 0; y < channelCount; y++)
		{
			DRDYNVC_SOFT_SYNC_ROUTE* route = &routes[routeCount++];
			Stream_Read_UINT32(s, route->channelId);
			route->tunnelType = tunnelType;
		}
	}

	if (Stream_GetRemainingLength(s) != 0)
		goto fail;

	if (routeCount > 1)
	{
		qsort(routes, routeCount, sizeof(*routes), drdynvc_soft_sync_route_compare);
		for (size_t x = 1; x < routeCount; x++)
		{
			if (routes[x - 1].channelId == routes[x].channelId)
				goto fail;
		}
	}

	for (size_t x = 0; x < routeCount; x++)
	{
		routes[x].channel =
		    dvcman_get_channel_by_id(drdynvc->channel_mgr, routes[x].channelId, TRUE);
		if (!routes[x].channel || (routes[x].channel->state != DVC_CHANNEL_RUNNING) ||
		    ((routes[x].tunnelType == TUNNELTYPE_UDPFECL) && routes[x].channel->dvc_data))
			goto fail;
	}

	DVCMAN* dvcman = (DVCMAN*)drdynvc->channel_mgr;
	wStream* response = StreamPool_Take(dvcman->pool, 6 + (size_t)numTunnels * 4);
	if (!response)
	{
		status = CHANNEL_RC_NO_MEMORY;
		goto fail;
	}

	Stream_Write_UINT8(response, (SOFT_SYNC_RESPONSE_PDU << 4));
	Stream_Write_UINT8(response, 0);
	Stream_Write_UINT32(response, numTunnels);
	for (UINT16 x = 0; x < numTunnels; x++)
		Stream_Write_UINT32(response, tunnelTypes[x]);

	EnterCriticalSection(&drdynvc->udpLock);
	if ((requestedMask != 0) &&
	    (!drdynvc->udpSend || ((drdynvc->udpTunnelMask & requestedMask) != requestedMask)))
	{
		LeaveCriticalSection(&drdynvc->udpLock);
		Stream_Release(response);
		status = ERROR_INVALID_STATE;
		goto fail;
	}

	status = drdynvc_send_strict(drdynvc, response, nullptr);
	if (status == CHANNEL_RC_OK)
	{
		drdynvc_clear_udp_routes_locked(drdynvc);
		/* The request lists define the server-write route. Use the same per-channel route as
		 * the deterministic client-write policy after accepting the tunnel types. */
		for (size_t x = 0; x < routeCount; x++)
			routes[x].channel->udpTunnelType = routes[x].tunnelType;
		drdynvc->udpNegotiatedMask = requestedMask;
		drdynvc->softSyncActive = TRUE;
	}
	LeaveCriticalSection(&drdynvc->udpLock);

	if (status != CHANNEL_RC_OK)
		goto fail;

	WLog_Print(drdynvc->log, WLOG_INFO,
	           "Soft-Sync accepted %" PRIu16 " tunnel(s) with %" PRIuz " channel route(s)",
	           numTunnels, routeCount);
	drdynvc_soft_sync_routes_free(routes, routeCount);
	return CHANNEL_RC_OK;

fail:
	WLog_Print(drdynvc->log, WLOG_WARN, "Rejecting invalid Soft-Sync request (status=%" PRIu32 ")",
	           status);
	drdynvc_soft_sync_routes_free(routes, routeCount);
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_order_recv(drdynvcPlugin* drdynvc, wStream* s, UINT32 ThreadingFlags)
{
	WINPR_ASSERT(drdynvc);
	if (!Stream_CheckAndLogRequiredLength(TAG, s, 1))
		return ERROR_INVALID_DATA;

	UINT8 value = Stream_Get_UINT8(s);
	const UINT8 Cmd = (value & 0xf0) >> 4;
	const UINT8 Sp = (value & 0x0c) >> 2;
	const UINT8 cbChId = (value & 0x03) >> 0;
	WLog_Print(drdynvc->log, WLOG_TRACE, "order_recv: Cmd=%s, Sp=%" PRIu8 " cbChId=%" PRIu8,
	           drdynvc_get_packet_type(Cmd), Sp, cbChId);

	switch (Cmd)
	{
		case CAPABILITY_REQUEST_PDU:
			return drdynvc_process_capability_request(drdynvc, Sp, cbChId, s);

		case CREATE_REQUEST_PDU:
			return drdynvc_process_create_request(drdynvc, Sp, cbChId, s);

		case DATA_FIRST_PDU:
		case DATA_FIRST_COMPRESSED_PDU:
			return drdynvc_process_data_first(drdynvc, Sp, cbChId, s,
			                                  (Cmd == DATA_FIRST_COMPRESSED_PDU), ThreadingFlags);

		case DATA_PDU:
		case DATA_COMPRESSED_PDU:
			return drdynvc_process_data(drdynvc, Sp, cbChId, s, (Cmd == DATA_COMPRESSED_PDU),
			                            ThreadingFlags);

		case CLOSE_REQUEST_PDU:
			return drdynvc_process_close_request(drdynvc, Sp, cbChId, s);

		case SOFT_SYNC_REQUEST_PDU:
			return drdynvc_process_soft_sync_request(drdynvc, Sp, cbChId, s);

		case SOFT_SYNC_RESPONSE_PDU:
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "not expecting a SOFT_SYNC_RESPONSE_PDU as a client");
			return ERROR_INTERNAL_ERROR;

		default:
			WLog_Print(drdynvc->log, WLOG_ERROR, "unknown drdynvc cmd 0x%x", Cmd);
			return ERROR_INTERNAL_ERROR;
	}
}

static UINT drdynvc_udp_get_channel_info(const BYTE* data, UINT32 length, UINT32* channelId,
                                         UINT8* command)
{
	WINPR_ASSERT(channelId);
	WINPR_ASSERT(command);
	if (!data || (length < 2))
		return ERROR_INVALID_PARAMETER;

	const UINT8 value = data[0];
	const UINT8 Cmd = (value & 0xf0) >> 4;
	const UINT8 Sp = (value & 0x0c) >> 2;
	const UINT8 cbChId = value & 0x03;
	if ((Cmd != DATA_FIRST_PDU) && (Cmd != DATA_FIRST_COMPRESSED_PDU) && (Cmd != DATA_PDU) &&
	    (Cmd != DATA_COMPRESSED_PDU))
		return ERROR_INVALID_DATA;
	if ((cbChId == 3) || (((Cmd == DATA_PDU) || (Cmd == DATA_COMPRESSED_PDU)) && (Sp != 0)) ||
	    (((Cmd == DATA_FIRST_PDU) || (Cmd == DATA_FIRST_COMPRESSED_PDU)) && (Sp == 3)))
		return ERROR_INVALID_DATA;

	wStream buffer = WINPR_C_ARRAY_INIT;
	wStream* s = Stream_StaticInit(&buffer, (BYTE*)&data[1], length - 1);
	if (!s || !Stream_CheckAndLogRequiredLength(TAG, s, drdynvc_cblen_to_bytes(cbChId)))
		return ERROR_INVALID_DATA;

	*channelId = drdynvc_read_variable_uint(s, cbChId);
	*command = Cmd;
	return CHANNEL_RC_OK;
}

static UINT drdynvc_process_udp_queue_item(drdynvcPlugin* drdynvc,
                                           const DRDYNVC_UDP_QUEUE_ITEM* item,
                                           BOOL discardRouteMismatch)
{
	WINPR_ASSERT(drdynvc);
	WINPR_ASSERT(item);
	WINPR_ASSERT(item->data);
	if (!drdynvc->channel_mgr)
		return discardRouteMismatch ? CHANNEL_RC_OK : ERROR_INVALID_STATE;

	EnterCriticalSection(&drdynvc->udpReceiveLock);
	DVCMAN_CHANNEL* channel = dvcman_get_channel_by_id(drdynvc->channel_mgr, item->channelId, TRUE);
	if (!channel)
	{
		LeaveCriticalSection(&drdynvc->udpReceiveLock);
		return discardRouteMismatch ? CHANNEL_RC_OK : ERROR_INVALID_DATA;
	}

	const UINT32 tunnelMask = drdynvc_udp_tunnel_mask(item->tunnelType);
	EnterCriticalSection(&drdynvc->udpLock);
	const BOOL routeMatches =
	    (item->bindGeneration || (drdynvc->udpGeneration == item->generation)) &&
	    drdynvc->softSyncActive && drdynvc->udpSend &&
	    ((drdynvc->udpTunnelMask & tunnelMask) != 0) &&
	    ((drdynvc->udpNegotiatedMask & tunnelMask) != 0) &&
	    (channel->udpTunnelType == item->tunnelType) && (channel->state == DVC_CHANNEL_RUNNING);
	LeaveCriticalSection(&drdynvc->udpLock);
	UINT status = CHANNEL_RC_OK;
	if (routeMatches)
	{
		/* UDP-L is an unreliable transport and MS-RDPEDYC forbids both fragmented
		 * and compressed DVC data on it. The public ingress rejects those commands
		 * before queueing; this second check protects the asynchronous boundary and
		 * ensures no pre-existing reassembly state can consume a single UDP-L DATA. */
		if ((item->tunnelType == TUNNELTYPE_UDPFECL) &&
		    ((item->command != DATA_PDU) || channel->dvc_data))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "Rejected invalid UDP-L DVC data for channel %" PRIu32, channel->channel_id);
			status = ERROR_INVALID_DATA;
		}
		else
			status = drdynvc_order_recv(drdynvc, item->data, TRUE);
	}
	dvcman_channel_unref(channel);
	LeaveCriticalSection(&drdynvc->udpReceiveLock);
	if (!routeMatches)
	{
		WLog_Print(drdynvc->log, WLOG_DEBUG,
		           "Discarded stale UDP DVC data for channel %" PRIu32 " generation %" PRIu64,
		           item->channelId, item->generation);
		return discardRouteMismatch ? CHANNEL_RC_OK : ERROR_INVALID_DATA;
	}

	return status;
}

UINT drdynvc_process_udp_data(DrdynvcClientContext* context, UINT32 tunnelType, const BYTE* data,
                              UINT32 length)
{
	UINT32 channelId = 0;
	UINT8 command = 0;
	drdynvcPlugin* drdynvc = drdynvc_get_plugin_from_context(context);
	const UINT32 tunnelMask = drdynvc_udp_tunnel_mask(tunnelType);
	if (!drdynvc || !data || (length == 0) || (length > CHANNEL_CHUNK_LENGTH) || (tunnelMask == 0))
		return ERROR_INVALID_PARAMETER;
	if (!drdynvc->channel_mgr)
		return ERROR_INVALID_STATE;

	UINT status = drdynvc_udp_get_channel_info(data, length, &channelId, &command);
	if (status != CHANNEL_RC_OK)
		return status;

	/* MS-RDPEDYC 3.1.5: UDP-L MUST NOT transport fragmented data. Compressed
	 * DVC PDUs also require a reliable transport. Consequently every UDP-L EMT
	 * message must contain exactly one uncompressed DYNVC_DATA PDU. */
	if ((tunnelType == TUNNELTYPE_UDPFECL) && (command != DATA_PDU))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR,
		           "Rejected DVC command 0x%02" PRIx8 " on unreliable UDP-L tunnel", command);
		return ERROR_INVALID_DATA;
	}

	DVCMAN* dvcman = (DVCMAN*)drdynvc->channel_mgr;
	if (!dvcman->pool)
		return ERROR_INVALID_STATE;

	wStream* s = StreamPool_Take(dvcman->pool, length);
	if (!s)
		return CHANNEL_RC_NO_MEMORY;
	Stream_Write(s, data, length);
	Stream_SealLength(s);
	Stream_ResetPosition(s);

	DRDYNVC_UDP_QUEUE_ITEM* item =
	    (DRDYNVC_UDP_QUEUE_ITEM*)calloc(1, sizeof(DRDYNVC_UDP_QUEUE_ITEM));
	if (!item)
	{
		Stream_Release(s);
		return CHANNEL_RC_NO_MEMORY;
	}
	item->data = s;
	item->tunnelType = tunnelType;
	item->channelId = channelId;
	item->command = command;

	if (drdynvc->async)
	{
		EnterCriticalSection(&drdynvc->udpLock);
		item->bindGeneration = !drdynvc->softSyncActive;
		if (!item->bindGeneration)
			item->generation = drdynvc->udpGeneration;
		LeaveCriticalSection(&drdynvc->udpLock);
		if (!drdynvc->queue ||
		    !MessageQueue_Post(drdynvc->queue, nullptr, DRDYNVC_QUEUE_UDP_PDU, item, nullptr))
		{
			Stream_Release(s);
			free(item);
			return ERROR_INTERNAL_ERROR;
		}
		return CHANNEL_RC_OK;
	}

	EnterCriticalSection(&drdynvc->udpLock);
	item->generation = drdynvc->udpGeneration;
	LeaveCriticalSection(&drdynvc->udpLock);
	status = drdynvc_process_udp_queue_item(drdynvc, item, FALSE);
	Stream_Release(s);
	free(item);
	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_virtual_channel_event_data_received(drdynvcPlugin* drdynvc, void* pData,
                                                        UINT32 dataLength, UINT32 totalLength,
                                                        UINT32 dataFlags)
{
	wStream* data_in = nullptr;

	WINPR_ASSERT(drdynvc);
	if ((dataFlags & CHANNEL_FLAG_SUSPEND) || (dataFlags & CHANNEL_FLAG_RESUME))
	{
		return CHANNEL_RC_OK;
	}

	if (dataFlags & CHANNEL_FLAG_FIRST)
	{
		DVCMAN* mgr = (DVCMAN*)drdynvc->channel_mgr;
		if (drdynvc->data_in)
			Stream_Release(drdynvc->data_in);

		drdynvc->data_in = StreamPool_Take(mgr->pool, totalLength);
	}

	if (!(data_in = drdynvc->data_in))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "StreamPool_Take failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	if (!Stream_EnsureRemainingCapacity(data_in, dataLength))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "Stream_EnsureRemainingCapacity failed!");
		Stream_Release(drdynvc->data_in);
		drdynvc->data_in = nullptr;
		return ERROR_INTERNAL_ERROR;
	}

	Stream_Write(data_in, pData, dataLength);

	if (dataFlags & CHANNEL_FLAG_LAST)
	{
		const size_t cap = Stream_Capacity(data_in);
		const size_t pos = Stream_GetPosition(data_in);
		if (cap < pos)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "drdynvc_plugin_process_received: read error");
			return ERROR_INVALID_DATA;
		}

		drdynvc->data_in = nullptr;
		Stream_SealLength(data_in);
		Stream_ResetPosition(data_in);

		if (drdynvc->async)
		{
			if (!MessageQueue_Post(drdynvc->queue, nullptr, DRDYNVC_QUEUE_TCP_PDU, (void*)data_in,
			                       nullptr))
			{
				WLog_Print(drdynvc->log, WLOG_ERROR, "MessageQueue_Post failed!");
				return ERROR_INTERNAL_ERROR;
			}
		}
		else
		{
			EnterCriticalSection(&drdynvc->udpReceiveLock);
			UINT error = drdynvc_order_recv(drdynvc, data_in, TRUE);
			LeaveCriticalSection(&drdynvc->udpReceiveLock);
			Stream_Release(data_in);

			if (error)
			{
				WLog_Print(drdynvc->log, WLOG_WARN,
				           "drdynvc_order_recv failed with error %" PRIu32 "!", error);
				return error;
			}
		}
	}

	return CHANNEL_RC_OK;
}

static void VCAPITYPE drdynvc_virtual_channel_open_event_ex(LPVOID lpUserParam, DWORD openHandle,
                                                            UINT event, LPVOID pData,
                                                            UINT32 dataLength, UINT32 totalLength,
                                                            UINT32 dataFlags)
{
	UINT error = CHANNEL_RC_OK;
	drdynvcPlugin* drdynvc = (drdynvcPlugin*)lpUserParam;

	WINPR_ASSERT(drdynvc);
	switch (event)
	{
		case CHANNEL_EVENT_DATA_RECEIVED:
			if (!drdynvc || (drdynvc->OpenHandle != openHandle))
			{
				WLog_ERR(TAG, "drdynvc_virtual_channel_open_event: error no match");
				return;
			}
			if ((error = drdynvc_virtual_channel_event_data_received(drdynvc, pData, dataLength,
			                                                         totalLength, dataFlags)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_data_received failed with error %" PRIu32
				           "",
				           error);

			break;

		case CHANNEL_EVENT_WRITE_CANCELLED:
		case CHANNEL_EVENT_WRITE_COMPLETE:
		{
			wStream* s = (wStream*)pData;
			Stream_Release(s);
		}
		break;

		case CHANNEL_EVENT_USER:
			break;
		default:
			break;
	}

	if (error && drdynvc && drdynvc->rdpcontext)
		setChannelError(drdynvc->rdpcontext, error,
		                "drdynvc_virtual_channel_open_event reported an error");
}

static DWORD WINAPI drdynvc_virtual_channel_client_thread(LPVOID arg)
{
	/* TODO: rewrite this */
	wStream* data = nullptr;
	wMessage message = WINPR_C_ARRAY_INIT;
	UINT error = CHANNEL_RC_OK;
	BOOL fatalUdpProtocolError = FALSE;
	drdynvcPlugin* drdynvc = (drdynvcPlugin*)arg;

	if (!drdynvc)
	{
		ExitThread((DWORD)CHANNEL_RC_BAD_CHANNEL_HANDLE);
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;
	}

	while (1)
	{
		if (!MessageQueue_Wait(drdynvc->queue))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "MessageQueue_Wait failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}

		if (!MessageQueue_Peek(drdynvc->queue, &message, TRUE))
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "MessageQueue_Peek failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}

		if (message.id == WMQ_QUIT)
			break;

		switch (message.id)
		{
			case DRDYNVC_QUEUE_TCP_PDU:
			{
				UINT32 ThreadingFlags = TRUE;
				data = (wStream*)message.wParam;

				EnterCriticalSection(&drdynvc->udpReceiveLock);
				if ((error = drdynvc_order_recv(drdynvc, data, ThreadingFlags)))
				{
					WLog_Print(drdynvc->log, WLOG_WARN,
					           "drdynvc_order_recv failed with error %" PRIu32 "!", error);
				}
				LeaveCriticalSection(&drdynvc->udpReceiveLock);

				Stream_Release(data);
				break;
			}

			case DRDYNVC_QUEUE_UDP_PDU:
			{
				DRDYNVC_UDP_QUEUE_ITEM* item = (DRDYNVC_UDP_QUEUE_ITEM*)message.wParam;
				if (item)
				{
					error = drdynvc_process_udp_queue_item(drdynvc, item, TRUE);
					if (error != CHANNEL_RC_OK)
					{
						WLog_Print(drdynvc->log, WLOG_WARN,
						           "UDP drdynvc_order_recv failed with error %" PRIu32 "!", error);
						fatalUdpProtocolError = item->tunnelType == TUNNELTYPE_UDPFECL;
					}
					Stream_Release(item->data);
					free(item);
				}
				break;
			}

			default:
				break;
		}

		if (fatalUdpProtocolError)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "Stopping DRDYNVC after an invalid UDP-L channel-data PDU");
			break;
		}
	}

	{
		/* Disconnect remaining dynamic channels that the server did not.
		 * This is required to properly shut down channels by calling the appropriate
		 * event handlers. */
		DVCMAN* drdynvcMgr = (DVCMAN*)drdynvc->channel_mgr;

		HashTable_Clear(drdynvcMgr->channelsById);
	}

	if (error && drdynvc->rdpcontext)
		setChannelError(drdynvc->rdpcontext, error,
		                "drdynvc_virtual_channel_client_thread reported an error");

	ExitThread((DWORD)error);
	return error;
}

static void drdynvc_queue_object_free(void* obj)
{
	wMessage* msg = (wMessage*)obj;
	if (!msg)
		return;

	switch (msg->id)
	{
		case DRDYNVC_QUEUE_TCP_PDU:
		{
			wStream* s = (wStream*)msg->wParam;
			if (s)
				Stream_Release(s);
			break;
		}

		case DRDYNVC_QUEUE_UDP_PDU:
		{
			DRDYNVC_UDP_QUEUE_ITEM* item = (DRDYNVC_UDP_QUEUE_ITEM*)msg->wParam;
			if (item)
			{
				if (item->data)
					Stream_Release(item->data);
				free(item);
			}
			break;
		}

		default:
			break;
	}
}

static UINT drdynvc_virtual_channel_event_initialized(drdynvcPlugin* drdynvc, LPVOID pData,
                                                      UINT32 dataLength)
{
	wObject* obj = nullptr;
	WINPR_UNUSED(pData);
	WINPR_UNUSED(dataLength);

	if (!drdynvc)
		goto error;

	drdynvc->queue = MessageQueue_New(nullptr);

	if (!drdynvc->queue)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "MessageQueue_New failed!");
		goto error;
	}

	obj = MessageQueue_Object(drdynvc->queue);
	obj->fnObjectFree = drdynvc_queue_object_free;
	drdynvc->channel_mgr = dvcman_new(drdynvc);

	if (!drdynvc->channel_mgr)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "dvcman_new failed!");
		goto error;
	}

	return CHANNEL_RC_OK;
error:
	return ERROR_INTERNAL_ERROR;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_virtual_channel_event_connected(drdynvcPlugin* drdynvc, LPVOID pData,
                                                    UINT32 dataLength)
{
	UINT error = 0;
	UINT32 status = 0;
	rdpSettings* settings = nullptr;

	WINPR_ASSERT(drdynvc);
	WINPR_UNUSED(pData);
	WINPR_UNUSED(dataLength);

	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	WINPR_ASSERT(drdynvc->channelEntryPoints.pVirtualChannelOpenEx);
	status = drdynvc->channelEntryPoints.pVirtualChannelOpenEx(
	    drdynvc->InitHandle, &drdynvc->OpenHandle, drdynvc->channelDef.name,
	    drdynvc_virtual_channel_open_event_ex);

	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "pVirtualChannelOpen failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(status), status);
		return status;
	}

	WINPR_ASSERT(drdynvc->rdpcontext);
	settings = drdynvc->rdpcontext->settings;
	WINPR_ASSERT(settings);

	for (UINT32 index = 0;
	     index < freerdp_settings_get_uint32(settings, FreeRDP_DynamicChannelCount); index++)
	{
		const ADDIN_ARGV* args =
		    freerdp_settings_get_pointer_array(settings, FreeRDP_DynamicChannelArray, index);
		error = dvcman_load_addin(drdynvc, drdynvc->channel_mgr, args, drdynvc->rdpcontext);

		if (CHANNEL_RC_OK != error)
			goto error;
	}

	if ((error = dvcman_init(drdynvc, drdynvc->channel_mgr)))
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "dvcman_init failed with error %" PRIu32 "!", error);
		goto error;
	}

	drdynvc->state = DRDYNVC_STATE_CAPABILITIES;

	if (drdynvc->async)
	{
		if (!(drdynvc->thread = CreateThread(nullptr, 0, drdynvc_virtual_channel_client_thread,
		                                     (void*)drdynvc, 0, nullptr)))
		{
			error = ERROR_INTERNAL_ERROR;
			WLog_Print(drdynvc->log, WLOG_ERROR, "CreateThread failed!");
			goto error;
		}

		if (!SetThreadPriority(drdynvc->thread, THREAD_PRIORITY_HIGHEST))
			WLog_Print(drdynvc->log, WLOG_WARN, "SetThreadPriority failed, ignoring.");
	}

error:
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_virtual_channel_event_disconnected(drdynvcPlugin* drdynvc)
{
	UINT status = 0;

	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	drdynvc_clear_udp_transport(drdynvc->context);

	if (drdynvc->OpenHandle == 0)
		return CHANNEL_RC_OK;

	if (drdynvc->queue)
	{
		if (!MessageQueue_PostQuit(drdynvc->queue, 0))
		{
			status = GetLastError();
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "MessageQueue_PostQuit failed with error %" PRIu32 "", status);
			return status;
		}
	}

	if (drdynvc->thread)
	{
		if (WaitForSingleObject(drdynvc->thread, INFINITE) != WAIT_OBJECT_0)
		{
			status = GetLastError();
			WLog_Print(drdynvc->log, WLOG_ERROR,
			           "WaitForSingleObject failed with error %" PRIu32 "", status);
			return status;
		}

		(void)CloseHandle(drdynvc->thread);
		drdynvc->thread = nullptr;
	}
	else
	{
		{
			/* Disconnect remaining dynamic channels that the server did not.
			 * This is required to properly shut down channels by calling the appropriate
			 * event handlers. */
			DVCMAN* drdynvcMgr = (DVCMAN*)drdynvc->channel_mgr;

			HashTable_Clear(drdynvcMgr->channelsById);
		}
	}

	WINPR_ASSERT(drdynvc->channelEntryPoints.pVirtualChannelCloseEx);
	status = drdynvc->channelEntryPoints.pVirtualChannelCloseEx(drdynvc->InitHandle,
	                                                            drdynvc->OpenHandle);

	if (status != CHANNEL_RC_OK)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "pVirtualChannelClose failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(status), status);
	}

	dvcman_clear(drdynvc, drdynvc->channel_mgr);
	if (drdynvc->queue)
		MessageQueue_Clear(drdynvc->queue);
	drdynvc->OpenHandle = 0;

	if (drdynvc->data_in)
	{
		Stream_Release(drdynvc->data_in);
		drdynvc->data_in = nullptr;
	}

	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT drdynvc_virtual_channel_event_terminated(drdynvcPlugin* drdynvc)
{
	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;
	drdynvc_clear_udp_transport(drdynvc->context);

	MessageQueue_Free(drdynvc->queue);
	drdynvc->queue = nullptr;

	if (drdynvc->channel_mgr)
	{
		dvcman_free(drdynvc, drdynvc->channel_mgr);
		drdynvc->channel_mgr = nullptr;
	}
	drdynvc->InitHandle = nullptr;
	if (drdynvc->context)
		drdynvc->context->handle = nullptr;
	if (drdynvc->udpReceiveLockInitialized)
	{
		DeleteCriticalSection(&drdynvc->udpReceiveLock);
		drdynvc->udpReceiveLockInitialized = FALSE;
	}
	if (drdynvc->udpLockInitialized)
	{
		DeleteCriticalSection(&drdynvc->udpLock);
		drdynvc->udpLockInitialized = FALSE;
	}
	free(drdynvc->context);
	free(drdynvc);
	return CHANNEL_RC_OK;
}

static UINT drdynvc_virtual_channel_event_attached(drdynvcPlugin* drdynvc)
{
	UINT error = CHANNEL_RC_OK;
	DVCMAN* dvcman = nullptr;

	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	dvcman = (DVCMAN*)drdynvc->channel_mgr;

	if (!dvcman)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	ArrayList_Lock(dvcman->plugins);
	for (size_t i = 0; i < ArrayList_Count(dvcman->plugins); i++)
	{
		IWTSPlugin* pPlugin = ArrayList_GetItem(dvcman->plugins, i);

		error = IFCALLRESULT(CHANNEL_RC_OK, pPlugin->Attached, pPlugin);
		if (error != CHANNEL_RC_OK)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "Attach failed with error %" PRIu32 "!", error);
			goto fail;
		}
	}

fail:
	ArrayList_Unlock(dvcman->plugins);
	return error;
}

static UINT drdynvc_virtual_channel_event_detached(drdynvcPlugin* drdynvc)
{
	UINT error = CHANNEL_RC_OK;
	DVCMAN* dvcman = nullptr;

	if (!drdynvc)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	dvcman = (DVCMAN*)drdynvc->channel_mgr;

	if (!dvcman)
		return CHANNEL_RC_BAD_CHANNEL_HANDLE;

	ArrayList_Lock(dvcman->plugins);
	for (size_t i = 0; i < ArrayList_Count(dvcman->plugins); i++)
	{
		IWTSPlugin* pPlugin = ArrayList_GetItem(dvcman->plugins, i);

		error = IFCALLRESULT(CHANNEL_RC_OK, pPlugin->Detached, pPlugin);
		if (error != CHANNEL_RC_OK)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "Detach failed with error %" PRIu32 "!", error);
			goto fail;
		}
	}

fail:
	ArrayList_Unlock(dvcman->plugins);

	return error;
}

static VOID VCAPITYPE drdynvc_virtual_channel_init_event_ex(LPVOID lpUserParam, LPVOID pInitHandle,
                                                            UINT event, LPVOID pData,
                                                            UINT dataLength)
{
	UINT error = CHANNEL_RC_OK;
	drdynvcPlugin* drdynvc = (drdynvcPlugin*)lpUserParam;

	if (!drdynvc || (drdynvc->InitHandle != pInitHandle))
	{
		WLog_ERR(TAG, "drdynvc_virtual_channel_init_event: error no match");
		return;
	}

	switch (event)
	{
		case CHANNEL_EVENT_INITIALIZED:
			error = drdynvc_virtual_channel_event_initialized(drdynvc, pData, dataLength);
			break;
		case CHANNEL_EVENT_CONNECTED:
			if ((error = drdynvc_virtual_channel_event_connected(drdynvc, pData, dataLength)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_connected failed with error %" PRIu32 "",
				           error);

			break;

		case CHANNEL_EVENT_DISCONNECTED:
			if ((error = drdynvc_virtual_channel_event_disconnected(drdynvc)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_disconnected failed with error %" PRIu32
				           "",
				           error);

			break;

		case CHANNEL_EVENT_TERMINATED:
			if ((error = drdynvc_virtual_channel_event_terminated(drdynvc)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_terminated failed with error %" PRIu32 "",
				           error);

			break;

		case CHANNEL_EVENT_ATTACHED:
			if ((error = drdynvc_virtual_channel_event_attached(drdynvc)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_attached failed with error %" PRIu32 "",
				           error);

			break;

		case CHANNEL_EVENT_DETACHED:
			if ((error = drdynvc_virtual_channel_event_detached(drdynvc)))
				WLog_Print(drdynvc->log, WLOG_ERROR,
				           "drdynvc_virtual_channel_event_detached failed with error %" PRIu32 "",
				           error);

			break;

		default:
			break;
	}

	if (error && drdynvc->rdpcontext)
		setChannelError(drdynvc->rdpcontext, error,
		                "drdynvc_virtual_channel_init_event_ex reported an error");
}

/**
 * Channel Client Interface
 */

static int drdynvc_get_version(DrdynvcClientContext* context)
{
	WINPR_ASSERT(context);
	drdynvcPlugin* drdynvc = (drdynvcPlugin*)context->handle;
	WINPR_ASSERT(drdynvc);
	return drdynvc->version;
}

/* drdynvc is always built-in */
#define VirtualChannelEntryEx drdynvc_VirtualChannelEntryEx

FREERDP_ENTRY_POINT(BOOL VCAPITYPE VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS_EX pEntryPoints,
                                                         PVOID pInitHandle))
{
	UINT rc = 0;
	drdynvcPlugin* drdynvc = nullptr;
	DrdynvcClientContext* context = nullptr;
	CHANNEL_ENTRY_POINTS_FREERDP_EX* pEntryPointsEx = nullptr;
	drdynvc = (drdynvcPlugin*)calloc(1, sizeof(drdynvcPlugin));

	WINPR_ASSERT(pEntryPoints);
	if (!drdynvc)
	{
		WLog_ERR(TAG, "calloc failed!");
		return FALSE;
	}
	drdynvc->log = WLog_Get(TAG);
	if (!InitializeCriticalSectionEx(&drdynvc->udpLock, 0, 0))
	{
		WLog_ERR(TAG, "InitializeCriticalSectionEx failed!");
		free(drdynvc);
		return FALSE;
	}
	drdynvc->udpLockInitialized = TRUE;
	if (!InitializeCriticalSectionEx(&drdynvc->udpReceiveLock, 0, 0))
	{
		WLog_ERR(TAG, "InitializeCriticalSectionEx failed!");
		DeleteCriticalSection(&drdynvc->udpLock);
		free(drdynvc);
		return FALSE;
	}
	drdynvc->udpReceiveLockInitialized = TRUE;

	drdynvc->channelDef.options =
	    CHANNEL_OPTION_INITIALIZED | CHANNEL_OPTION_ENCRYPT_RDP | CHANNEL_OPTION_COMPRESS_RDP;
	(void)sprintf_s(drdynvc->channelDef.name, ARRAYSIZE(drdynvc->channelDef.name),
	                DRDYNVC_SVC_CHANNEL_NAME);
	drdynvc->state = DRDYNVC_STATE_INITIAL;
	pEntryPointsEx = (CHANNEL_ENTRY_POINTS_FREERDP_EX*)pEntryPoints;

	if ((pEntryPointsEx->cbSize >= sizeof(CHANNEL_ENTRY_POINTS_FREERDP_EX)) &&
	    (pEntryPointsEx->MagicNumber == FREERDP_CHANNEL_MAGIC_NUMBER))
	{
		context = (DrdynvcClientContext*)calloc(1, sizeof(DrdynvcClientContext));

		if (!context)
		{
			WLog_Print(drdynvc->log, WLOG_ERROR, "calloc failed!");
			DeleteCriticalSection(&drdynvc->udpReceiveLock);
			DeleteCriticalSection(&drdynvc->udpLock);
			free(drdynvc);
			return FALSE;
		}

		context->handle = (void*)drdynvc;
		context->custom = nullptr;
		drdynvc->context = context;
		context->GetVersion = drdynvc_get_version;
		context->GetChannelStats = drdynvc_get_channel_stats;
		drdynvc->rdpcontext = pEntryPointsEx->context;
		if (!freerdp_settings_get_bool(drdynvc->rdpcontext->settings,
		                               FreeRDP_TransportDumpReplay) &&
		    !freerdp_settings_get_bool(drdynvc->rdpcontext->settings,
		                               FreeRDP_SynchronousDynamicChannels))
			drdynvc->async = TRUE;
	}

	WLog_Print(drdynvc->log, WLOG_DEBUG, "VirtualChannelEntryEx");
	CopyMemory(&(drdynvc->channelEntryPoints), pEntryPoints,
	           sizeof(CHANNEL_ENTRY_POINTS_FREERDP_EX));
	drdynvc->InitHandle = pInitHandle;

	WINPR_ASSERT(drdynvc->channelEntryPoints.pVirtualChannelInitEx);
	rc = drdynvc->channelEntryPoints.pVirtualChannelInitEx(
	    drdynvc, context, pInitHandle, &drdynvc->channelDef, 1, VIRTUAL_CHANNEL_VERSION_WIN2000,
	    drdynvc_virtual_channel_init_event_ex);

	if (CHANNEL_RC_OK != rc)
	{
		WLog_Print(drdynvc->log, WLOG_ERROR, "pVirtualChannelInit failed with %s [%08" PRIX32 "]",
		           WTSErrorToString(rc), rc);
		free(drdynvc->context);
		DeleteCriticalSection(&drdynvc->udpReceiveLock);
		DeleteCriticalSection(&drdynvc->udpLock);
		free(drdynvc);
		return FALSE;
	}

	drdynvc->channelEntryPoints.pInterface = context;
	return TRUE;
}

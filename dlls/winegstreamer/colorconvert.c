/* GStreamer Color Converter
 *
 * Copyright 2020 Derek Lesho
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "gst_private.h"

#include "mfapi.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

static const GUID *raw_types[] = {
    &MFVideoFormat_RGB24,
    &MFVideoFormat_RGB32,
    &MFVideoFormat_RGB555,
    &MFVideoFormat_RGB8,
    &MFVideoFormat_AYUV,
    &MFVideoFormat_I420,
    &MFVideoFormat_IYUV,
    &MFVideoFormat_NV11,
    &MFVideoFormat_NV12,
    &MFVideoFormat_UYVY,
    &MFVideoFormat_v216,
    &MFVideoFormat_v410,
    &MFVideoFormat_YUY2,
    &MFVideoFormat_YVYU,
    &MFVideoFormat_YVYU,
};

struct color_converter
{
    IMFTransform IMFTransform_iface;
    LONG refcount;
    IMFMediaType *input_type;
    IMFMediaType *output_type;
    CRITICAL_SECTION cs;
};

static struct color_converter *impl_color_converter_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct color_converter, IMFTransform_iface);
}

static HRESULT WINAPI color_converter_QueryInterface(IMFTransform *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IMFTransform) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFTransform_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI color_converter_AddRef(IMFTransform *iface)
{
    struct color_converter *transform = impl_color_converter_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&transform->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI color_converter_Release(IMFTransform *iface)
{
    struct color_converter *transform = impl_color_converter_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&transform->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        transform->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&transform->cs);
        if (transform->output_type)
            IMFMediaType_Release(transform->output_type);
        free(transform);
    }

    return refcount;
}

static HRESULT WINAPI color_converter_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum, DWORD *input_maximum,
        DWORD *output_minimum, DWORD *output_maximum)
{
    TRACE("%p, %p, %p, %p, %p.\n", iface, input_minimum, input_maximum, output_minimum, output_maximum);

    *input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;

    return S_OK;
}

static HRESULT WINAPI color_converter_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    TRACE("%p, %p, %p.\n", iface, inputs, outputs);

    *inputs = *outputs = 1;

    return S_OK;
}

static HRESULT WINAPI color_converter_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    TRACE("%p %u %p %u %p.\n", iface, input_size, inputs, output_size, outputs);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    UINT64 framesize;
    GUID subtype;

    TRACE("%p %u %p.\n", iface, id, info);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_DOES_NOT_ADDREF | MFT_INPUT_STREAM_FIXED_SAMPLE_SIZE;
    info->cbMaxLookahead = 0;
    info->cbAlignment = 0;
    info->hnsMaxLatency = 0;
    info->cbSize = 0;

    EnterCriticalSection(&converter->cs);

    if (converter->input_type)
    {
        if (SUCCEEDED(IMFMediaType_GetGUID(converter->input_type, &MF_MT_SUBTYPE, &subtype)) &&
            SUCCEEDED(IMFMediaType_GetUINT64(converter->input_type, &MF_MT_FRAME_SIZE, &framesize)))
        {
            MFCalculateImageSize(&subtype, framesize >> 32, (UINT32) framesize, &info->cbSize);
        }

        if (!info->cbSize)
            WARN("Failed to get desired input buffer size, the non-provided sample path will likely break\n");
    }

    LeaveCriticalSection(&converter->cs);

    return S_OK;
}

static HRESULT WINAPI color_converter_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    UINT64 framesize;
    GUID subtype;

    TRACE("%p %u %p.\n", iface, id, info);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    info->dwFlags = MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES | MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
    info->cbAlignment = 0;
    info->cbSize = 0;

    EnterCriticalSection(&converter->cs);

    if (converter->output_type)
    {
        if (SUCCEEDED(IMFMediaType_GetGUID(converter->output_type, &MF_MT_SUBTYPE, &subtype)) &&
            SUCCEEDED(IMFMediaType_GetUINT64(converter->output_type, &MF_MT_FRAME_SIZE, &framesize)))
        {
            MFCalculateImageSize(&subtype, framesize >> 32, (UINT32) framesize, &info->cbSize);
        }

        if (!info->cbSize)
            WARN("Failed to get desired output buffer size, the non-provided sample path will likely break\n");
    }

    LeaveCriticalSection(&converter->cs);

    return S_OK;
}

static HRESULT WINAPI color_converter_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    FIXME("%p, %p.\n", iface, attributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetInputStreamAttributes(IMFTransform *iface, DWORD id,
        IMFAttributes **attributes)
{
    FIXME("%p, %u, %p.\n", iface, id, attributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetOutputStreamAttributes(IMFTransform *iface, DWORD id,
        IMFAttributes **attributes)
{
    FIXME("%p, %u, %p.\n", iface, id, attributes);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    TRACE("%p, %u.\n", iface, id);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    TRACE("%p, %u, %p.\n", iface, streams, ids);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    IMFMediaType *ret;
    HRESULT hr;

    TRACE("%p, %u, %u, %p.\n", iface, id, index, type);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (index >= ARRAY_SIZE(raw_types))
        return MF_E_NO_MORE_TYPES;

    if (FAILED(hr = MFCreateMediaType(&ret)))
        return hr;

    if (FAILED(hr = IMFMediaType_SetGUID(ret, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)))
    {
        IMFMediaType_Release(ret);
        return hr;
    }

    if (FAILED(hr = IMFMediaType_SetGUID(ret, &MF_MT_SUBTYPE, raw_types[index])))
    {
        IMFMediaType_Release(ret);
        return hr;
    }

    *type = ret;

    return S_OK;
}

static HRESULT WINAPI color_converter_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    IMFMediaType *ret;
    HRESULT hr;

    TRACE("%p, %u, %u, %p.\n", iface, id, index, type);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (index >= ARRAY_SIZE(raw_types))
        return MF_E_NO_MORE_TYPES;

    if (FAILED(hr = MFCreateMediaType(&ret)))
        return hr;

    EnterCriticalSection(&converter->cs);

    if (converter->input_type)
        IMFMediaType_CopyAllItems(converter->input_type, (IMFAttributes *) ret);

    LeaveCriticalSection(&converter->cs);

    if (FAILED(hr = IMFMediaType_SetGUID(ret, &MF_MT_MAJOR_TYPE, &MFMediaType_Video)))
    {
        IMFMediaType_Release(ret);
        return hr;
    }

    if (FAILED(hr = IMFMediaType_SetGUID(ret, &MF_MT_SUBTYPE, raw_types[index])))
    {
        IMFMediaType_Release(ret);
        return hr;
    }

    *type = ret;

    return S_OK;
}

static HRESULT WINAPI color_converter_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    UINT64 input_framesize, output_framesize;
    GUID major_type, subtype;
    unsigned int i;
    HRESULT hr;

    TRACE("%p, %u, %p, %#x.\n", iface, id, type, flags);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!type)
    {
        if (flags & MFT_SET_TYPE_TEST_ONLY)
            return S_OK;

        EnterCriticalSection(&converter->cs);

        if (converter->input_type)
        {
            IMFMediaType_Release(converter->input_type);
            converter->input_type = NULL;
        }

        LeaveCriticalSection(&converter->cs);

        return S_OK;
    }

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major_type)))
        return MF_E_INVALIDTYPE;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDTYPE;

    if (!IsEqualGUID(&major_type, &MFMediaType_Video))
        return MF_E_INVALIDTYPE;

    for (i = 0; i < ARRAY_SIZE(raw_types); i++)
    {
        if (IsEqualGUID(&subtype, raw_types[i]))
            break;
    }

    if (i == ARRAY_SIZE(raw_types))
        return MF_E_INVALIDTYPE;

    EnterCriticalSection(&converter->cs);

    if(converter->output_type
         && SUCCEEDED(IMFMediaType_GetUINT64(converter->output_type, &MF_MT_FRAME_SIZE, &output_framesize))
         && SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &input_framesize))
         && input_framesize != output_framesize)
    {
        LeaveCriticalSection(&converter->cs);
        return MF_E_INVALIDTYPE;
    }

    LeaveCriticalSection(&converter->cs);

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    EnterCriticalSection(&converter->cs);

    hr = S_OK;

    if (!converter->input_type)
        hr = MFCreateMediaType(&converter->input_type);

    if (SUCCEEDED(hr))
        hr = IMFMediaType_CopyAllItems(type, (IMFAttributes *) converter->input_type);

    if (FAILED(hr))
    {
        IMFMediaType_Release(converter->input_type);
        converter->input_type = NULL;
    }

    LeaveCriticalSection(&converter->cs);

    return hr;
}

static HRESULT WINAPI color_converter_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    UINT64 input_framesize, output_framesize;
    GUID major_type, subtype;
    unsigned int i;
    HRESULT hr;

    TRACE("%p, %u, %p, %#x.\n", iface, id, type, flags);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (!type)
    {
        if (flags & MFT_SET_TYPE_TEST_ONLY)
            return S_OK;

        EnterCriticalSection(&converter->cs);

        if (converter->output_type)
        {
            IMFMediaType_Release(converter->output_type);
            converter->output_type = NULL;
        }

        LeaveCriticalSection(&converter->cs);

        return S_OK;
    }

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major_type)))
        return MF_E_INVALIDTYPE;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDTYPE;

    if (!IsEqualGUID(&major_type, &MFMediaType_Video))
        return MF_E_INVALIDTYPE;

    for (i = 0; i < ARRAY_SIZE(raw_types); i++)
    {
        if (IsEqualGUID(&subtype, raw_types[i]))
            break;
    }

    if (i == ARRAY_SIZE(raw_types))
        return MF_E_INVALIDTYPE;

    EnterCriticalSection(&converter->cs);

    if(converter->input_type
         && SUCCEEDED(IMFMediaType_GetUINT64(converter->input_type, &MF_MT_FRAME_SIZE, &input_framesize))
         && SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &output_framesize))
         && input_framesize != output_framesize)
    {
        LeaveCriticalSection(&converter->cs);
        return MF_E_INVALIDTYPE;
    }

    LeaveCriticalSection(&converter->cs);

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    EnterCriticalSection(&converter->cs);

    hr = S_OK;

    if (!converter->output_type)
        hr = MFCreateMediaType(&converter->output_type);

    if (SUCCEEDED(hr))
        hr = IMFMediaType_CopyAllItems(type, (IMFAttributes *) converter->output_type);

    if (FAILED(hr))
    {
        IMFMediaType_Release(converter->output_type);
        converter->output_type = NULL;
    }

    LeaveCriticalSection(&converter->cs);

    return S_OK;
}

static HRESULT WINAPI color_converter_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    FIXME("%p, %u, %p.\n", iface, id, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    struct color_converter *converter = impl_color_converter_from_IMFTransform(iface);
    IMFMediaType *ret;
    HRESULT hr;

    TRACE("%p, %u, %p.\n", converter, id, type);

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;

    if (FAILED(hr = MFCreateMediaType(&ret)))
        return hr;

    EnterCriticalSection(&converter->cs);

    if (converter->output_type)
        hr = IMFMediaType_CopyAllItems(converter->output_type, (IMFAttributes *)ret);
    else
        hr = MF_E_TRANSFORM_TYPE_NOT_SET;

    LeaveCriticalSection(&converter->cs);

    if (SUCCEEDED(hr))
        *type = ret;
    else
        IMFMediaType_Release(ret);

    return hr;
}

static HRESULT WINAPI color_converter_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    FIXME("%p, %u, %p.\n", iface, id, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    FIXME("%p, %p.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    FIXME("%p, %s, %s.\n", iface, wine_dbgstr_longlong(lower), wine_dbgstr_longlong(upper));

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    TRACE("%p, %u, %p.\n", iface, id, event);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    TRACE("%p, %u %lu.\n", iface, message, param);

    switch(message)
    {
        case MFT_MESSAGE_COMMAND_FLUSH:
        case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
            return S_OK;
        default:
            FIXME("Unhandled message type %x.\n", message);
            return E_NOTIMPL;
    }
}

static HRESULT WINAPI color_converter_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    FIXME("%p, %u, %p, %#x.\n", iface, id, sample, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI color_converter_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    FIXME("%p, %#x, %u, %p, %p.\n", iface, flags, count, samples, status);

    return E_NOTIMPL;
}

static const IMFTransformVtbl color_converter_vtbl =
{
    color_converter_QueryInterface,
    color_converter_AddRef,
    color_converter_Release,
    color_converter_GetStreamLimits,
    color_converter_GetStreamCount,
    color_converter_GetStreamIDs,
    color_converter_GetInputStreamInfo,
    color_converter_GetOutputStreamInfo,
    color_converter_GetAttributes,
    color_converter_GetInputStreamAttributes,
    color_converter_GetOutputStreamAttributes,
    color_converter_DeleteInputStream,
    color_converter_AddInputStreams,
    color_converter_GetInputAvailableType,
    color_converter_GetOutputAvailableType,
    color_converter_SetInputType,
    color_converter_SetOutputType,
    color_converter_GetInputCurrentType,
    color_converter_GetOutputCurrentType,
    color_converter_GetInputStatus,
    color_converter_GetOutputStatus,
    color_converter_SetOutputBounds,
    color_converter_ProcessEvent,
    color_converter_ProcessMessage,
    color_converter_ProcessInput,
    color_converter_ProcessOutput,
};

HRESULT color_converter_create(REFIID riid, void **ret)
{
    struct color_converter *object;

    TRACE("%s %p\n", debugstr_guid(riid), ret);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFTransform_iface.lpVtbl = &color_converter_vtbl;
    object->refcount = 1;

    InitializeCriticalSection(&object->cs);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": color_converter_lock");

    *ret = &object->IMFTransform_iface;
    return S_OK;
}
// Generated with hyprwayland-scanner 0.4.6. Made with vaxry's keyboard and ❤️.
// ext_data_control_v1

/*
 This protocol's authors' copyright notice is:


    Copyright © 2018 Simon Ser
    Copyright © 2019 Ivan Molodetskikh
    Copyright © 2024 Neal Gompa

    Permission to use, copy, modify, distribute, and sell this
    software and its documentation for any purpose is hereby granted
    without fee, provided that the above copyright notice appear in
    all copies and that both that copyright notice and this permission
    notice appear in supporting documentation, and that the name of
    the copyright holders not be used in advertising or publicity
    pertaining to distribution of the software without specific,
    written prior permission.  The copyright holders make no
    representations about the suitability of this software for any
    purpose.  It is provided "as is" without express or implied
    warranty.

    THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
    SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
    FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
    SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
    AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
    ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
    THIS SOFTWARE.
  
*/

#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <wayland-client.h>

#define F std::function

struct wl_proxy;

enum extDataControlDeviceV1Error : uint32_t {
    EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE = 1,
};

enum extDataControlSourceV1Error : uint32_t {
    EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER = 1,
};


class CCExtDataControlManagerV1;
class CCExtDataControlSourceV1;
class CCExtDataControlDeviceV1;
class CCWlSeat;
class CCExtDataControlDeviceV1;
class CCExtDataControlSourceV1;
class CCExtDataControlSourceV1;
class CCExtDataControlOfferV1;
class CCExtDataControlOfferV1;
class CCExtDataControlOfferV1;
class CCExtDataControlSourceV1;
class CCExtDataControlOfferV1;

#ifndef HYPRWAYLAND_SCANNER_NO_INTERFACES
extern const wl_interface ext_data_control_manager_v1_interface;
extern const wl_interface ext_data_control_device_v1_interface;
extern const wl_interface ext_data_control_source_v1_interface;
extern const wl_interface ext_data_control_offer_v1_interface;

#endif


class CCExtDataControlManagerV1 {
  public:
    CCExtDataControlManagerV1(wl_proxy*);
    ~CCExtDataControlManagerV1();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    wl_proxy* sendCreateDataSource();
    wl_proxy* sendGetDataDevice(wl_proxy*);
    void sendDestroy();

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCExtDataControlDeviceV1 {
  public:
    CCExtDataControlDeviceV1(wl_proxy*);
    ~CCExtDataControlDeviceV1();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setDataOffer(F<void(CCExtDataControlDeviceV1*, wl_proxy*)> &&handler);
    void setSelection(F<void(CCExtDataControlDeviceV1*, wl_proxy*)> &&handler);
    void setFinished(F<void(CCExtDataControlDeviceV1*)> &&handler);
    void setPrimarySelection(F<void(CCExtDataControlDeviceV1*, wl_proxy*)> &&handler);

    // --------------- Events --------------- //

    void sendSetSelection(CCExtDataControlSourceV1*);
    void sendDestroy();
    void sendSetPrimarySelection(CCExtDataControlSourceV1*);

  private:
    struct {
        F<void(CCExtDataControlDeviceV1*, wl_proxy*)> dataOffer;
        F<void(CCExtDataControlDeviceV1*, wl_proxy*)> selection;
        F<void(CCExtDataControlDeviceV1*)> finished;
        F<void(CCExtDataControlDeviceV1*, wl_proxy*)> primarySelection;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCExtDataControlSourceV1 {
  public:
    CCExtDataControlSourceV1(wl_proxy*);
    ~CCExtDataControlSourceV1();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setSend(F<void(CCExtDataControlSourceV1*, const char*, int32_t)> &&handler);
    void setCancelled(F<void(CCExtDataControlSourceV1*)> &&handler);

    // --------------- Events --------------- //

    void sendOffer(const char*);
    void sendDestroy();

  private:
    struct {
        F<void(CCExtDataControlSourceV1*, const char*, int32_t)> send;
        F<void(CCExtDataControlSourceV1*)> cancelled;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCExtDataControlOfferV1 {
  public:
    CCExtDataControlOfferV1(wl_proxy*);
    ~CCExtDataControlOfferV1();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setOffer(F<void(CCExtDataControlOfferV1*, const char*)> &&handler);

    // --------------- Events --------------- //

    void sendReceive(const char*, int32_t);
    void sendDestroy();

  private:
    struct {
        F<void(CCExtDataControlOfferV1*, const char*)> offer;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



#undef F

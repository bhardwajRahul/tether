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

#define private public
#define HYPRWAYLAND_SCANNER_NO_INTERFACES
#include "ext-data-control-v1.hpp"
#undef private
#define F std::function

static const wl_interface* extDataControlV1_dummyTypes[] = {nullptr};

// Reference all other interfaces.
// The reason why this is in snake is to
// be able to cooperate with existing
// wayland_scanner interfaces (they are interop)
extern const wl_interface ext_data_control_manager_v1_interface;
extern const wl_interface ext_data_control_device_v1_interface;
extern const wl_interface ext_data_control_source_v1_interface;
extern const wl_interface ext_data_control_offer_v1_interface;
extern const wl_interface wl_seat_interface;

static const void* _CCExtDataControlManagerV1VTable[] = {
    nullptr,
};

wl_proxy* CCExtDataControlManagerV1::sendCreateDataSource() {
    if (!pResource)
        return nullptr;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 0, &ext_data_control_source_v1_interface, wl_proxy_get_version(pResource), 0, nullptr);

    return proxy;
}

wl_proxy* CCExtDataControlManagerV1::sendGetDataDevice(wl_proxy* seat) {
    if (!pResource)
        return nullptr;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 1, &ext_data_control_device_v1_interface, wl_proxy_get_version(pResource), 0, nullptr, seat);

    return proxy;
}

void CCExtDataControlManagerV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 2, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CExtDataControlManagerV1CreateDataSourceTypes[] = {
    &ext_data_control_source_v1_interface,
};
static const wl_interface* _CExtDataControlManagerV1GetDataDeviceTypes[] = {
    &ext_data_control_device_v1_interface,
    &wl_seat_interface,
};

static const wl_message _CExtDataControlManagerV1Requests[] = {
    {.name = "create_data_source", .signature = "n", .types = _CExtDataControlManagerV1CreateDataSourceTypes + 0},
    {.name = "get_data_device", .signature = "no", .types = _CExtDataControlManagerV1GetDataDeviceTypes + 0},
    {.name = "destroy", .signature = "", .types = extDataControlV1_dummyTypes + 0},
};

const wl_interface ext_data_control_manager_v1_interface = {
    .name = "ext_data_control_manager_v1",
    .version = 1,
    .method_count = 3,
    .methods = _CExtDataControlManagerV1Requests,
    .event_count = 0,
    .events = nullptr,
};

CCExtDataControlManagerV1::CCExtDataControlManagerV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCExtDataControlManagerV1VTable, this);
}

CCExtDataControlManagerV1::~CCExtDataControlManagerV1() {
    if (!destroyed)
        sendDestroy();
}

static void _CExtDataControlDeviceV1DataOffer(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCExtDataControlDeviceV1*)data;
    if (PO && PO->requests.dataOffer)
        PO->requests.dataOffer(PO, id);
}

static void _CExtDataControlDeviceV1Selection(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCExtDataControlDeviceV1*)data;
    if (PO && PO->requests.selection)
        PO->requests.selection(PO, id);
}

static void _CExtDataControlDeviceV1Finished(void* data, void* resource) {
    const auto PO = (CCExtDataControlDeviceV1*)data;
    if (PO && PO->requests.finished)
        PO->requests.finished(PO);
}

static void _CExtDataControlDeviceV1PrimarySelection(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCExtDataControlDeviceV1*)data;
    if (PO && PO->requests.primarySelection)
        PO->requests.primarySelection(PO, id);
}

static const void* _CCExtDataControlDeviceV1VTable[] = {
    (void*)_CExtDataControlDeviceV1DataOffer,
    (void*)_CExtDataControlDeviceV1Selection,
    (void*)_CExtDataControlDeviceV1Finished,
    (void*)_CExtDataControlDeviceV1PrimarySelection,
};

void CCExtDataControlDeviceV1::sendSetSelection(CCExtDataControlSourceV1* source) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, source ? source->pResource : nullptr);
    proxy;
}

void CCExtDataControlDeviceV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}

void CCExtDataControlDeviceV1::sendSetPrimarySelection(CCExtDataControlSourceV1* source) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 2, nullptr, wl_proxy_get_version(pResource), 0, source ? source->pResource : nullptr);
    proxy;
}
static const wl_interface* _CExtDataControlDeviceV1SetSelectionTypes[] = {
    &ext_data_control_source_v1_interface,
};
static const wl_interface* _CExtDataControlDeviceV1SetPrimarySelectionTypes[] = {
    &ext_data_control_source_v1_interface,
};
static const wl_interface* _CExtDataControlDeviceV1DataOfferTypes[] = {
    &ext_data_control_offer_v1_interface,
};
static const wl_interface* _CExtDataControlDeviceV1SelectionTypes[] = {
    &ext_data_control_offer_v1_interface,
};
static const wl_interface* _CExtDataControlDeviceV1PrimarySelectionTypes[] = {
    &ext_data_control_offer_v1_interface,
};

static const wl_message _CExtDataControlDeviceV1Requests[] = {
    {.name = "set_selection", .signature = "?o", .types = _CExtDataControlDeviceV1SetSelectionTypes + 0},
    {.name = "destroy", .signature = "", .types = extDataControlV1_dummyTypes + 0},
    {.name = "set_primary_selection", .signature = "?o", .types = _CExtDataControlDeviceV1SetPrimarySelectionTypes + 0},
};

static const wl_message _CExtDataControlDeviceV1Events[] = {
    {.name = "data_offer", .signature = "n", .types = _CExtDataControlDeviceV1DataOfferTypes + 0},
    {.name = "selection", .signature = "?o", .types = _CExtDataControlDeviceV1SelectionTypes + 0},
    {.name = "finished", .signature = "", .types = extDataControlV1_dummyTypes + 0},
    {.name = "primary_selection", .signature = "?o", .types = _CExtDataControlDeviceV1PrimarySelectionTypes + 0},
};

const wl_interface ext_data_control_device_v1_interface = {
    .name = "ext_data_control_device_v1",
    .version = 1,
    .method_count = 3,
    .methods = _CExtDataControlDeviceV1Requests,
    .event_count = 4,
    .events = _CExtDataControlDeviceV1Events,
};

CCExtDataControlDeviceV1::CCExtDataControlDeviceV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCExtDataControlDeviceV1VTable, this);
}

CCExtDataControlDeviceV1::~CCExtDataControlDeviceV1() {
    if (!destroyed)
        sendDestroy();
}

void CCExtDataControlDeviceV1::setDataOffer(F<void(CCExtDataControlDeviceV1*, wl_proxy*)>&& handler) {
    requests.dataOffer = std::move(handler);
}

void CCExtDataControlDeviceV1::setSelection(F<void(CCExtDataControlDeviceV1*, wl_proxy*)>&& handler) {
    requests.selection = std::move(handler);
}

void CCExtDataControlDeviceV1::setFinished(F<void(CCExtDataControlDeviceV1*)>&& handler) {
    requests.finished = std::move(handler);
}

void CCExtDataControlDeviceV1::setPrimarySelection(F<void(CCExtDataControlDeviceV1*, wl_proxy*)>&& handler) {
    requests.primarySelection = std::move(handler);
}

static void _CExtDataControlSourceV1Send(void* data, void* resource, const char* mime_type, int32_t fd) {
    const auto PO = (CCExtDataControlSourceV1*)data;
    if (PO && PO->requests.send)
        PO->requests.send(PO, mime_type, fd);
}

static void _CExtDataControlSourceV1Cancelled(void* data, void* resource) {
    const auto PO = (CCExtDataControlSourceV1*)data;
    if (PO && PO->requests.cancelled)
        PO->requests.cancelled(PO);
}

static const void* _CCExtDataControlSourceV1VTable[] = {
    (void*)_CExtDataControlSourceV1Send,
    (void*)_CExtDataControlSourceV1Cancelled,
};

void CCExtDataControlSourceV1::sendOffer(const char* mime_type) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, mime_type);
    proxy;
}

void CCExtDataControlSourceV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CExtDataControlSourceV1OfferTypes[] = {
    nullptr,
};
static const wl_interface* _CExtDataControlSourceV1SendTypes[] = {
    nullptr,
    nullptr,
};

static const wl_message _CExtDataControlSourceV1Requests[] = {
    {.name = "offer", .signature = "s", .types = _CExtDataControlSourceV1OfferTypes + 0},
    {.name = "destroy", .signature = "", .types = extDataControlV1_dummyTypes + 0},
};

static const wl_message _CExtDataControlSourceV1Events[] = {
    {.name = "send", .signature = "sh", .types = _CExtDataControlSourceV1SendTypes + 0},
    {.name = "cancelled", .signature = "", .types = extDataControlV1_dummyTypes + 0},
};

const wl_interface ext_data_control_source_v1_interface = {
    .name = "ext_data_control_source_v1",
    .version = 1,
    .method_count = 2,
    .methods = _CExtDataControlSourceV1Requests,
    .event_count = 2,
    .events = _CExtDataControlSourceV1Events,
};

CCExtDataControlSourceV1::CCExtDataControlSourceV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCExtDataControlSourceV1VTable, this);
}

CCExtDataControlSourceV1::~CCExtDataControlSourceV1() {
    if (!destroyed)
        sendDestroy();
}

void CCExtDataControlSourceV1::setSend(F<void(CCExtDataControlSourceV1*, const char*, int32_t)>&& handler) {
    requests.send = std::move(handler);
}

void CCExtDataControlSourceV1::setCancelled(F<void(CCExtDataControlSourceV1*)>&& handler) {
    requests.cancelled = std::move(handler);
}

static void _CExtDataControlOfferV1Offer(void* data, void* resource, const char* mime_type) {
    const auto PO = (CCExtDataControlOfferV1*)data;
    if (PO && PO->requests.offer)
        PO->requests.offer(PO, mime_type);
}

static const void* _CCExtDataControlOfferV1VTable[] = {
    (void*)_CExtDataControlOfferV1Offer,
};

void CCExtDataControlOfferV1::sendReceive(const char* mime_type, int32_t fd) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, mime_type, fd);
    proxy;
}

void CCExtDataControlOfferV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CExtDataControlOfferV1ReceiveTypes[] = {
    nullptr,
    nullptr,
};
static const wl_interface* _CExtDataControlOfferV1OfferTypes[] = {
    nullptr,
};

static const wl_message _CExtDataControlOfferV1Requests[] = {
    {.name = "receive", .signature = "sh", .types = _CExtDataControlOfferV1ReceiveTypes + 0},
    {.name = "destroy", .signature = "", .types = extDataControlV1_dummyTypes + 0},
};

static const wl_message _CExtDataControlOfferV1Events[] = {
    {.name = "offer", .signature = "s", .types = _CExtDataControlOfferV1OfferTypes + 0},
};

const wl_interface ext_data_control_offer_v1_interface = {
    .name = "ext_data_control_offer_v1",
    .version = 1,
    .method_count = 2,
    .methods = _CExtDataControlOfferV1Requests,
    .event_count = 1,
    .events = _CExtDataControlOfferV1Events,
};

CCExtDataControlOfferV1::CCExtDataControlOfferV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCExtDataControlOfferV1VTable, this);
}

CCExtDataControlOfferV1::~CCExtDataControlOfferV1() {
    if (!destroyed)
        sendDestroy();
}

void CCExtDataControlOfferV1::setOffer(F<void(CCExtDataControlOfferV1*, const char*)>&& handler) {
    requests.offer = std::move(handler);
}

#undef F

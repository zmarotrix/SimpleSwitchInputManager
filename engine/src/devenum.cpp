#include "devenum.h"

#include <initguid.h>
#include <devpkey.h>

std::vector<std::wstring> InterfaceList(const GUID& guid, const wchar_t* instanceId)
{
    std::vector<std::wstring> out;
    for (int attempt = 0; attempt < 3; attempt++) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&guid, (DEVINSTID_W)instanceId,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
            return out;
        }
        std::vector<wchar_t> buf(len + 1, 0);
        CONFIGRET cr = CM_Get_Device_Interface_ListW((LPGUID)&guid, (DEVINSTID_W)instanceId, buf.data(), len,
                                                     CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL) continue;  // a device arrived between the two calls
        if (cr != CR_SUCCESS) return out;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
        return out;
    }
    return out;
}

std::wstring InterfaceInstanceId(const std::wstring& path)
{
    wchar_t id[MAX_DEVICE_ID_LEN];
    ULONG size = sizeof(id);
    DEVPROPTYPE type;
    if (CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId, &type, (PBYTE)id, &size, 0) !=
        CR_SUCCESS) {
        return L"";
    }
    return id;
}

std::wstring NodeId(DEVINST dn)
{
    wchar_t id[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return L"";
    return id;
}

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle)
{
    std::wstring h = hay, n = needle;
    _wcsupr_s(h.data(), h.size() + 1);
    _wcsupr_s(n.data(), n.size() + 1);
    return h.find(n) != std::wstring::npos;
}


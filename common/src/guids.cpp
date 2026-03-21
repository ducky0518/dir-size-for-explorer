// Instantiate all project GUIDs and property keys as actual symbols.
// We define them explicitly to avoid initguid.h / propkeydef.h include-order issues.

#include <Windows.h>
#include <propsys.h>

// {B7E8A3D1-5C4F-4A92-8E1D-6F3B2A9C0D4E}
extern "C" const GUID DECLSPEC_SELECTANY FMTID_DirSize =
    { 0xb7e8a3d1, 0x5c4f, 0x4a92, { 0x8e, 0x1d, 0x6f, 0x3b, 0x2a, 0x9c, 0x0d, 0x4e } };

// {A1C3E5F7-2B4D-6E8F-9A0B-C1D2E3F4A5B6}
extern "C" const GUID DECLSPEC_SELECTANY CLSID_DirSizePropertyHandler =
    { 0xa1c3e5f7, 0x2b4d, 0x6e8f, { 0x9a, 0x0b, 0xc1, 0xd2, 0xe3, 0xf4, 0xa5, 0xb6 } };

// {D2E4F6A8-3C5E-7F90-AB12-D3E4F5A6B7C8}
extern "C" const GUID DECLSPEC_SELECTANY CLSID_DirSizeContextMenu =
    { 0xd2e4f6a8, 0x3c5e, 0x7f90, { 0xab, 0x12, 0xd3, 0xe4, 0xf5, 0xa6, 0xb7, 0xc8 } };

// {F3A5C7E9-4D6F-8A1B-BC23-E4F5A6B7C8D9}
extern "C" const GUID DECLSPEC_SELECTANY CLSID_DirSizeOverlay =
    { 0xf3a5c7e9, 0x4d6f, 0x8a1b, { 0xbc, 0x23, 0xe4, 0xf5, 0xa6, 0xb7, 0xc8, 0xd9 } };

// PKEY_DirSize_TotalSize: {B7E8A3D1-5C4F-4A92-8E1D-6F3B2A9C0D4E}, 2
extern "C" const PROPERTYKEY DECLSPEC_SELECTANY PKEY_DirSize_TotalSize =
    { { 0xb7e8a3d1, 0x5c4f, 0x4a92, { 0x8e, 0x1d, 0x6f, 0x3b, 0x2a, 0x9c, 0x0d, 0x4e } }, 2 };

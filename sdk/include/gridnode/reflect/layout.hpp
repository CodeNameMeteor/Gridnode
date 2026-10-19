#pragma once
// Frostbite reflection layouts for the Mirror's Edge Catalyst PC build.
//
// All of these were confirmed against the PC Scylla dump by FrostbiteTypeDumper
// (calibration output) and, where noted, in game by MECMP. Image VAs assume the
// image base 0x140000000.

#include <cstdint>

namespace gn::layout {

// --- Globals ------------------------------------------------------------------------------------
constexpr uint64_t kImageBase = 0x140000000ull;
constexpr uint64_t kTypeInfoListHead = 0x14215d028ull; // TypeInfo* g_firstTypeInfo

// --- Runtime TypeInfo ---------------------------------------------------------------------------
constexpr uint32_t TI_Data = 0x00;      // TypeInfoData*
constexpr uint32_t TI_Next = 0x08;      // TypeInfo* (global singly linked list)
constexpr uint32_t TI_RuntimeId = 0x10; // u16, unique per type
constexpr uint32_t TI_Flags = 0x12;     // u16

// --- Runtime ClassInfo (extends TypeInfo) -------------------------------------------------------
constexpr uint32_t CI_Super = 0x18;           // ClassInfo*
constexpr uint32_t CI_DefaultInstance = 0x20; // heap object (per session)
constexpr uint32_t CI_ClassId = 0x28;         // u16; isKindOf: (id - first) <= (last - first)
constexpr uint32_t CI_LastSubClassId = 0x2A;  // u16
constexpr uint32_t CI_FirstDerived = 0x30;    // ClassInfo*
constexpr uint32_t CI_NextSibling = 0x38;     // ClassInfo*
// Data classes keep their vtable at +0x50. Runtime classes keep a creator for realm 0 (client)
// at +0x50 and realm 1 (server) at +0x58. Some ClassInfos are only 0x50 or 0x58 bytes long.
constexpr uint32_t CI_Slot50 = 0x50;
constexpr uint32_t CI_Slot58 = 0x58;
constexpr uint32_t CI_SizeShort = 0x50;
constexpr uint32_t CI_SizeLong = 0x60;

// --- Entity creator (what CI_Slot50/58 point at for runtime classes) ------------------------------
constexpr uint32_t Creator_Vtable = 0x00;  // slot +0x08 is the factory (creator, EntityCreationInfo*)
constexpr uint32_t Creator_DataTi = 0x18;  // TypeInfo* of the data class it creates from
constexpr uint32_t Creator_Realm = 0x24;   // i32: 0 client, 1 server

// --- Static TypeInfoData ------------------------------------------------------------------------
constexpr uint32_t TID_Name = 0x00;       // const char*
constexpr uint32_t TID_Flags = 0x08;      // u16: bits 4..8 = type code
constexpr uint32_t TID_Size = 0x0A;       // u16
constexpr uint32_t TID_Module = 0x10;     // ModuleInfo* (name at +0)
constexpr uint32_t TID_Alignment = 0x18;  // u8
constexpr uint32_t TID_FieldCount = 0x1A; // u16 (enum value count for enums)
constexpr uint32_t TID_ClassSuper = 0x20;      // ClassInfo*
constexpr uint32_t TID_ClassCreate = 0x28;     // create function
constexpr uint32_t TID_ClassFields = 0x30;     // FieldInfoData*
constexpr uint32_t TID_ValueTypeInstance = 0x20; // messages: static instance (vtable at +0)
constexpr uint32_t TID_ValueTypeFields = 0x28;   // FieldInfoData*
constexpr uint32_t TID_EnumFields = 0x20;      // FieldInfoData*
constexpr uint32_t TID_ArrayElement = 0x20;    // TypeInfo*

// --- FieldInfoData (0x18 bytes) -----------------------------------------------------------------
constexpr uint32_t FID_Stride = 0x18;
constexpr uint32_t FID_Name = 0x00;   // const char*
constexpr uint32_t FID_Flags = 0x08;  // u16: 0x0048 array field, 0x2000 settings field
constexpr uint32_t FID_Offset = 0x0A; // u16
constexpr uint32_t FID_Type = 0x10;   // TypeInfo*; for enum values the low 32 bits are the value

// --- Virtual functions ------------------------------------------------------------------------------
constexpr uint32_t kGetTypeSlot = 0; // getType() is vtable slot 0 on PC

inline constexpr uint32_t typeCode(uint16_t flags) { return (flags >> 4) & 0x1f; }

} // namespace gn::layout

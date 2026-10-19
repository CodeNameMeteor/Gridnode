// Frostbite reflection (TypeInfo) dumper for Mirror's Edge Catalyst (PS4 eboot / PC dump).
//
// Frostbite registers every reflected type (classes, value types, enums,
// arrays, primitives) from C++ static initializers. Field tables are filled in
// at runtime, so a purely static scan misses most of the data. This script
// instead EMULATES the static initializers with Ghidra's p-code emulator, then
// walks the engine's global TypeInfo linked list inside the emulated memory and
// writes out:
//
//   <outdir>/fb_types.txt   human readable dump (every type + fields)
//   <outdir>/fb_types.json  machine readable dump (for other tools/agents)
//   <outdir>/fb_types.h     C++ header with offset-accurate structs
//   <outdir>/fb_types_calibration.txt  raw layout evidence to double check offsets
//
// Optionally annotates the Ghidra database:
//   - data types in /fb (one struct per Class/ValueType, one enum per Enum, opaque structs
//     for primitives such as the 0x20-byte String), /fb/vtables (X_vtbl) and the reflection
//     records in /fb/reflection (TypeInfo, ClassInfoData, FieldInfoData...)
//   - those record types applied to every TypeInfo, TypeInfoData, field table and vftable
//   - labels fb::TI_<Name>, fb::TID_<Name>, fb::FID_<Name>
//   - fb::<Class>::getType (typed ClassInfo* getType(fb::X* this)), ::create, ::vftable.
//     getType is found by byte pattern; the vftable is the one the create function stores
//     whose getType slot holds this class's getType.
//   - functions found in exactly one vtable moved into that class (vf<slot>), __thiscall
//     where the compiler spec has it, so Ghidra types `this` as fb::X*
// Re-running replaces the script's own labels and types instead of piling up duplicates.
// Names and signatures from you (USER_DEFINED) or from symbols (IMPORTED) are never changed.
//
// Nothing is written to the program bytes: emulated writes stay in the
// emulator's memory state.
//
// Usage (GUI):      Script Manager -> Frostbite -> FrostbiteTypeDumper
// Usage (headless): analyzeHeadless <proj> <name> -process eboot.bin -noanalysis \
//                     -postScript FrostbiteTypeDumper.java /path/to/outdir [noannotate]
//
// Addresses below are for the PS4 Mirror's Edge Catalyst eboot loaded at
// 0x01000000 (see research.md) and the PC Scylla dump. For another build,
// update the profile constants.
//
//@category Frostbite
//@author MECMP

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.math.BigInteger;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.emulator.EmulatorHelper;
import ghidra.app.script.GhidraScript;
import ghidra.pcode.emulate.BreakCallBack;
import ghidra.pcode.memstate.MemoryFaultHandler;
import ghidra.pcode.pcoderaw.PcodeOpRaw;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.BooleanDataType;
import ghidra.program.model.data.ByteDataType;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.EnumDataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.LongLongDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.QWordDataType;
import ghidra.program.model.data.ShortDataType;
import ghidra.program.model.data.SignedByteDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.UnsignedLongLongDataType;
import ghidra.program.model.data.UnsignedShortDataType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolType;
import ghidra.util.exception.CancelledException;

public class FrostbiteTypeDumper extends GhidraScript {

	// ------------------------------------------------------------------
	// PROFILES. These are constants; the active profile is copied into instance
	// fields at the start of every run, so running on the PC dump and then on the
	// PS4 eboot in the same Ghidra session can't leak addresses between them.
	// ------------------------------------------------------------------

	// ---- PS4 profile (eboot, image base 0x01000000) ----

	/** Head of the global singly linked list of runtime TypeInfo objects. */
	static final long PS4_ADDR_FIRST_TYPEINFO = 0x04578fb0L;

	/** TypeInfo base ctor: (TypeInfo* ti, TypeInfoData* data, u16 flags). */
	static final long PS4_FN_TYPEINFO_ASSIGN_ID = 0x0108d7f0L;

	/** Links a TypeInfo into the global list: (TypeInfo* ti). */
	static final long PS4_FN_TYPEINFO_LINK = 0x0108d810L;

	/**
	 * Registration helpers -> category. The category of a type is taken from
	 * the helper that called FN_TYPEINFO_ASSIGN_ID / FN_TYPEINFO_LINK for it.
	 * Unknown helpers fall back to flag/name heuristics.
	 */
	static final Object[][] PS4_HELPERS = {
		{ 0x0107b620L, "Class" },     // fb_ClassInfo_ctor
		{ 0x010710e0L, "Array" },     // array-of-class TypeInfo (vtable object, TypeInfo at +8)
		{ 0x0106e470L, "Array" },     // array-of-valuetype TypeInfo
		{ 0x0108bda0L, "Array" },     // array-of-enum TypeInfo (inferred)
		{ 0x0108b5e0L, "Enum" },      // EnumTypeInfo ctor (inferred)
		{ 0x0108c8e0L, "ValueType" }, // ValueTypeInfo ctor (confirmed: links inline, no call to the primitives)
		{ 0x01071ed0L, "Class" },     // ClassInfo ctor for runtime (non-DataContainer) classes (confirmed)
	};

	/** Functions that allocate memory: {address, index of size argument (0=1st,1=2nd,2=3rd)}. */
	static final long[][] PS4_ALLOCATORS = {
		{ 0x0105b110L, 0 }, // (size, arena)
		{ 0x01055390L, 1 }, // (arena, size, align, ...)
	};

	// ---- PC profile (MirrorsEdgeCatalyst.exe, Scylla dump of the running game) ----
	// Found 2026-10-06 from the PamReplayPlayerComponentData registration (FUN_1447461b0).
	static final long PC_ADDR_FIRST_TYPEINFO = 0x14215d028L;
	static final long PC_FN_TYPEINFO_ASSIGN_ID = 0x142a3d400L;
	static final long PC_FN_TYPEINFO_LINK = 0x142a46480L;
	static final Object[][] PC_HELPERS = {
		{ 0x142a82440L, "Class" }, // ClassInfo ctor (confirmed)
	};
	/** Not found yet: emulating a non-snapshot PE runs straight into the real allocator. */
	static final long[][] PC_ALLOCATORS = {};

	/**
	 * Slot of the virtual getType() in a vtable, counted from the address point the
	 * constructor stores. PC: 0 (verified on EntityData, vtable 0x141aef5c8). PS4: -1 =
	 * derive it from the create functions (and, failing that, a slot histogram).
	 */
	static final int PS4_GETTYPE_SLOT = -1;
	static final int PC_GETTYPE_SLOT = 0;

	/**
	 * Offset from a primitive TypeInfo to its Array<primitive> TypeInfo, used to recover
	 * array types whose registration was lost in emulation. PS4 only: on PC primitive
	 * TypeInfos are 0x20 apart, so primitive+0x20 is the next primitive. 0 disables the rule.
	 */
	static final int PS4_PRIMITIVE_ARRAY_OFF = 0x20;
	static final int PC_PRIMITIVE_ARRAY_OFF = 0;

	/** FieldInfoData flag values seen in Catalyst. */
	static final int FIELD_FLAG_ARRAY = 0x0048;
	static final int FIELD_FLAG_SETTING = 0x2000;

	// TypeInfoData layout (confirmed from the first dump's calibration evidence):
	//   +0x00 name, +0x08 u16 flags, +0x0A u16 size, +0x10 module, +0x18 u8 alignment,
	//   +0x1A u16 fieldCount, +0x20 kind-specific (Class: super / Enum: fields / Array: element),
	//   Class: +0x28 create fn, +0x30 fields; the record is 0x38 bytes (the next record starts
	//   at +0x38 on PC). ValueType: +0x20 unknown pointer, +0x28 fields.
	// TypeInfoData flags: bits 0-1 member type, bits 2-3 category (1 Class, 2 ValueType,
	//   3 Primitive), bits 4-8 type code (all codes confirmed against the PC dump).
	// FieldInfoData flags do NOT carry a type code in Catalyst: only 0x0000, 0x0048 (array
	//   field) and 0x2000 (settings / console-exposed) occur. Field types come from the
	//   field's TypeInfo.
	static final int TID_MODULE_OFF = 0x10;
	static final int TID_ALIGN_OFF = 0x18;
	static final int TID_FIELD_COUNT_OFF = 0x1A;
	static final int CLASS_FIELDS_PTR_OFF = 0x30;
	static final int CLASS_DATA_SUPER_OFF = 0x20;
	static final int CLASS_CREATE_FN_OFF = 0x28;
	static final int VALUETYPE_FIELDS_PTR_OFF = 0x28;
	static final int ENUM_FIELDS_PTR_OFF = 0x20;
	static final int ARRAY_ELEMENT_OFF = 0x20;

	/** Anything below this is a null-ish pointer (e.g. obj+8 on a null object). */
	static final long MIN_VALID_ADDR = 0x10000L;

	/** Instructions the emulator cannot model are skipped, up to this many per initializer. */
	static final int MAX_SKIPS_PER_ROOT = 256;

	/** Offset of the super ClassInfo* inside a runtime ClassInfo (confirmed). */
	static final int CLASS_RUNTIME_SUPER_OFF = 0x18;

	// Runtime TypeInfo / ClassInfo layout, read from the PC dump's raw records (2026-10-08).
	// Non-class TypeInfos are 0x20 bytes apart. The ClassInfo fields are validated against
	// the super-class tree on every run (see crossCheck) before they are used.
	static final int TI_RUNTIME_ID_OFF = 0x10;
	static final int CI_DEFAULT_INSTANCE_OFF = 0x20;
	static final int CI_CLASS_ID_OFF = 0x28;
	static final int CI_FIRST_DERIVED_OFF = 0x30;
	/**
	 * ClassInfo+0x50 / +0x58: data classes hold their vtable at +0x50; runtime classes hold a
	 * creator object for realm 0 at +0x50 and realm 1 at +0x58. Creator layout, from its
	 * constructor (PC FUN_14315fed0): +0x00 vtable, +0x08 prev, +0x10 next, +0x18 TypeInfo*
	 * of the data class it creates from, +0x20 u32, +0x24 int realm, +0x2C u8 registered.
	 * Both readings are checked per pointer before use.
	 */
	static final int CI_SLOT50_OFF = 0x50;
	static final int CREATOR_DATA_TI_OFF = 0x18;
	static final int CREATOR_REALM_OFF = 0x24;
	static final int CREATOR_SIZE = 0x30;
	static final int CLASSINFO_SIZE = 0x50;
	static final int CLASSINFO_EX_SIZE = 0x60;
	/** ValueTypeInfoData+0x20: static instance (messages: first qword is the message vtable). */
	static final int VALUETYPE_INSTANCE_OFF = 0x20;

	/** FieldInfoData layout (confirmed): name@0, flags@8 (u16), offset@0xA (u16), type@0x10. */
	static final int FIELD_STRIDE = 0x18;

	static final long MAX_STEPS_PER_ROOT = 500_000L;
	/** Call-graph search depth when guessing roots (no init array available). */
	static final int MAX_CALL_DEPTH_SEARCH = 3;
	/** Call-graph search depth when the roots come from the init array (no false roots to fear). */
	static final int MAX_CALL_DEPTH_INIT_ARRAY = 6;

	// Scratch regions used by the emulator. Must not overlap program memory.
	static final long STACK_TOP = 0x7ff00000L;
	/** Room above the initial stack pointer (Win64 callees write 0x20 bytes of shadow space there). */
	static final long STACK_HEADROOM = 0x1000L;
	static final long FAKE_HEAP_BASE = 0x50000000L;
	static final long FAKE_HEAP_END = 0x5ff00000L;
	static final long SENTINEL_RET = 0x0feed000L;

	static final String[] RESET_REGS = {
		"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
		"XMM0", "XMM1", "XMM2", "XMM3", "XMM4", "XMM5", "XMM6", "XMM7",
		"XMM8", "XMM9", "XMM10", "XMM11", "XMM12", "XMM13", "XMM14", "XMM15",
		"CF", "PF", "AF", "ZF", "SF", "DF", "OF" };

	/** Fake import copies/fills are clamped to this many bytes. */
	static final int MAX_FAKE_COPY = 1 << 20;

	/** Data type categories used for annotation. Class structs live in /fb so fb::X class namespaces find them. */
	static final String FB_CATEGORY = "/fb";
	static final String REFLECTION_CATEGORY = "/fb/reflection";
	static final String VTABLE_CATEGORY = "/fb/vtables";

	// ------------------------------------------------------------------
	// Active profile (set per run)
	// ------------------------------------------------------------------

	private long addrFirstTypeInfo;
	private long fnAssignId;
	private long fnLink;
	private Object[][] helpers;
	private long[][] allocators;
	/** Integer argument registers: SysV (PS4) or Windows x64. */
	private String[] argRegs;
	private int profileGetTypeSlot;
	private int primitiveArrayOff;
	private boolean isPe;

	// ------------------------------------------------------------------

	private EmulatorHelper emu;
	private Memory mem;
	private long heapCursor = FAKE_HEAP_BASE;
	private boolean heapWrapped;
	private MemoryBlock lastBlock;

	/** runtime TypeInfo -> registration record. */
	private final Map<Long, Reg> regs = new HashMap<>();
	private final Map<Long, String> helperCategory = new HashMap<>();
	private final Map<Long, Integer> allocatorSizeArg = new HashMap<>();

	/** pc -> (instruction length << 1 | hasStore), or -1 when the listing has no instruction there. */
	private final Map<Long, Integer> insInfoCache = new HashMap<>();
	/** pc -> name of the external function a thunk at pc jumps to, or "" if pc is not such a thunk. */
	private final Map<Long, String> importThunkCache = new HashMap<>();

	private final List<String> rootLog = new ArrayList<>();
	private String rootSource = "";
	private long lastHead;
	private int totalSkips;

	/** Slot of getType() in the vtable (most common index), or -1 if not found. */
	private int getTypeVtableIndex = -1;
	private final Map<String, Integer> annotateStats = new TreeMap<>();

	static class Reg {
		long ti;
		long data;
		long helper; // entry of the function that registered it (0 if unknown)
	}

	static class Field {
		String name;
		int flags;
		int offset;
		long typeTi;
		long raw0C;

		boolean isArrayFlag() {
			return (flags & FIELD_FLAG_ARRAY) == FIELD_FLAG_ARRAY;
		}
	}

	static class TypeRec {
		long ti;
		long data;
		String name = "";
		String ident = ""; // unique C/Ghidra identifier, assigned once all types are known
		int flags;
		int size;
		int alignment;
		String category = "Unknown";
		long superTi;
		long elementTi;
		long fieldsPtr;
		int fieldsPtrOff = -1;
		List<Field> fields = new ArrayList<>();
		long helper;
		String note = ""; // e.g. "inferred" when recovered without emulated registration
		String module = "";
		String helperCategory = ""; // category the registration helper implies, if known
		long getTypeFn;   // virtual getType() (in this type's vtable at the getType slot)
		List<Long> typeAccessors = new ArrayList<>(); // all "return &TypeInfo" functions for this type
		List<Long> vftables = new ArrayList<>();
		boolean vftableConfirmed; // vftable confirmed by a vtable store (create fn or static instance)
		long createFn;

		// Runtime TypeInfo (all types): +0x10 u16 runtime id, +0x12 u16 unknown.
		int runtimeId = -1;
		int unk12;
		// Runtime ClassInfo (Classes; validated in crossCheck before use):
		// +0x20 default instance (heap, data classes only), +0x28 u16 classId,
		// +0x2A u16 lastSubClassId, +0x30 first derived class, +0x38 next sibling class.
		long defaultInstance;
		int classId = -1;
		int lastSubClassId = -1;
		long firstDerived;
		long nextSibling;
		long slot50;
		long slot58;
		// ValueTypeInfoData+0x20, raw. For messages it is a static instance whose first qword
		// is the message vtable (staticInstance, set once that is proven); for other value
		// types it points next to their array TypeInfo and is not decoded.
		long valueTypeUnk20;
		long staticInstance;
		/** Creators (runtime classes): {creator object, realm, data class TypeInfo}. */
		List<long[]> creators = new ArrayList<>();
		/** Data classes: runtime classes created from this one. */
		List<Long> createdBy = new ArrayList<>();
	}

	private final Map<Long, TypeRec> types = new LinkedHashMap<>();

	@Override
	protected void run() throws Exception {
		mem = currentProgram.getMemory();
		boolean windows = selectProfile();
		for (Object[] h : helpers) {
			helperCategory.put((Long) h[0], (String) h[1]);
		}
		for (long[] a : allocators) {
			allocatorSizeArg.put(a[0], (int) a[1]);
		}

		File outDir = chooseOutputDir();
		boolean annotate = chooseAnnotate();

		List<Function> roots = new ArrayList<>(); // filled below unless the image is a memory snapshot

		emu = new EmulatorHelper(currentProgram);
		try {
			emu.setMemoryFaultHandler(new MemoryFaultHandler() {
				@Override
				public boolean uninitializedRead(Address address, int size, byte[] buf, int bufOffset) {
					// .bss and scratch memory read as zero.
					return true;
				}

				@Override
				public boolean unknownAddress(Address address, boolean write) {
					return false;
				}
			});
			// Unknown p-code ops (rare AVX helpers) become no-ops instead of aborting.
			emu.registerDefaultCallOtherCallback(new BreakCallBack() {
				@Override
				public boolean pcodeCallback(PcodeOpRaw op) {
					// Give the op's output a defined value so later reads do not fault.
					try {
						Varnode out = op.getOutput();
						if (out != null) {
							emu.getEmulator().getMemState().setValue(out, 0L);
						}
					}
					catch (Throwable t) {
						// best effort
					}
					return true;
				}
			});
			int ok = 0;
			int done = 0;
			int preListed = countListInMemory();
			if (preListed <= 100) {
				// Not a snapshot under the configured head: check the profile before
				// spending minutes emulating against a wrong address.
				preListed = validateProfile(preListed);
			}
			lastHead = readQ(addrFirstTypeInfo);
			boolean snapshot = preListed > 100;
			if (snapshot) {
				rootSource = "memory snapshot: initializers not emulated";
				println("FrostbiteTypeDumper: " + preListed + " types already on the list in this image "
					+ "(memory snapshot): skipping emulation");
			}
			else {
				roots = findInitializerRoots();
				println("FrostbiteTypeDumper: " + roots.size() + " static initializers (" + rootSource + ")");
				if (allocators.length == 0) {
					printerr("FrostbiteTypeDumper: WARNING no allocators in this profile: initializers that allocate "
						+ "will run the game's real allocator and may abort. A memory snapshot avoids emulation.");
				}
			}
			for (Function f : roots) {
				if (monitor.isCancelled()) {
					throw new CancelledException();
				}
				monitor.setMessage("Emulating " + f.getName() + " (" + (++done) + "/" + roots.size() + ")");
				String result;
				try {
					result = emulateRoot(f);
				}
				catch (CancelledException e) {
					throw e;
				}
				catch (Exception e) {
					// One bad initializer must not lose the whole dump.
					result = "abort: " + e.getClass().getSimpleName() + ": " + e.getMessage();
				}
				if (result.startsWith("ok")) {
					ok++;
				}
				rootLog.add(f.getEntryPoint() + " " + f.getName() + " : " + result);
			}
			if (!snapshot) {
				println("FrostbiteTypeDumper: emulated " + ok + "/" + roots.size() + " initializers cleanly ("
					+ (windows ? "Windows x64" : "SysV") + " calling convention)");
				println("FrostbiteTypeDumper: skipped " + totalSkips + " unmodellable instructions in total");
			}
			collectTypes();
			if (types.isEmpty() || !looksLikeTypeInfo(readQ(addrFirstTypeInfo))) {
				printerr("FrostbiteTypeDumper: the TypeInfo list at " + hex(addrFirstTypeInfo) + " is empty after "
					+ (snapshot ? "loading" : "emulation") + ". Check the profile addresses (list head, "
					+ "registration functions) for this build. Emulation log:");
				for (String s : rootLog.subList(0, Math.min(20, rootLog.size()))) {
					printerr("  " + s);
				}
				return;
			}
			for (TypeRec t : new ArrayList<>(types.values())) {
				processType(t);
			}
			discoverReferencedTypes();
			repairPrimitiveTable();
			crossCheck();
			assignIdents();
			findReflectionMethods();

			println("FrostbiteTypeDumper: " + types.size() + " types recovered");

			writeText(new File(outDir, "fb_types.txt"));
			writeJson(new File(outDir, "fb_types.json"));
			writeHeader(new File(outDir, "fb_types.h"));

			if (annotate) {
				createDataTypes();
				annotateProgram();
			}
			writeCalibration(new File(outDir, "fb_types_calibration.txt"));
			println("FrostbiteTypeDumper: wrote output to " + outDir.getAbsolutePath());
		}
		finally {
			emu.dispose();
		}
	}

	/** Copies the PS4 or PC profile into the instance fields. Returns true for the Windows x64 ABI. */
	private boolean selectProfile() {
		String fmt = String.valueOf(currentProgram.getExecutableFormat());
		String cspec = "";
		try {
			cspec = currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString();
		}
		catch (Exception e) {
			// fall back on the executable format alone
		}
		isPe = fmt.contains("Portable Executable") || fmt.contains("PE");
		boolean windows = isPe || cspec.equalsIgnoreCase("windows");
		if (isPe) {
			addrFirstTypeInfo = PC_ADDR_FIRST_TYPEINFO;
			fnAssignId = PC_FN_TYPEINFO_ASSIGN_ID;
			fnLink = PC_FN_TYPEINFO_LINK;
			helpers = PC_HELPERS;
			allocators = PC_ALLOCATORS;
			profileGetTypeSlot = PC_GETTYPE_SLOT;
			primitiveArrayOff = PC_PRIMITIVE_ARRAY_OFF;
		}
		else {
			addrFirstTypeInfo = PS4_ADDR_FIRST_TYPEINFO;
			fnAssignId = PS4_FN_TYPEINFO_ASSIGN_ID;
			fnLink = PS4_FN_TYPEINFO_LINK;
			helpers = PS4_HELPERS;
			allocators = PS4_ALLOCATORS;
			profileGetTypeSlot = PS4_GETTYPE_SLOT;
			primitiveArrayOff = PS4_PRIMITIVE_ARRAY_OFF;
		}
		argRegs = windows ? new String[] { "RCX", "RDX", "R8" } : new String[] { "RDI", "RSI", "RDX" };
		println("FrostbiteTypeDumper: format '" + fmt + "', compiler spec '" + cspec + "' -> "
			+ (isPe ? "PC profile" : "PS4 profile") + ", list head " + hex(addrFirstTypeInfo));
		return windows;
	}

	/**
	 * Sanity checks for the profile, run when the configured list head does not already hold
	 * a populated list. The link function writes the list head, so a head that the link
	 * function never references is almost certainly wrong; if the link function writes exactly
	 * one data address, that is used instead. Returns the (possibly new) pre-listed count.
	 */
	private int validateProfile(int preListed) {
		FunctionManager fm = currentProgram.getFunctionManager();
		if (fm.getFunctionAt(toAddr(fnAssignId)) == null) {
			printerr("FrostbiteTypeDumper: WARNING no function at the TypeInfo ctor " + hex(fnAssignId));
		}
		Function link = fm.getFunctionAt(toAddr(fnLink));
		if (link == null) {
			printerr("FrostbiteTypeDumper: WARNING no function at the TypeInfo link function " + hex(fnLink)
				+ ": cannot check the list head");
			return preListed;
		}
		Set<Long> written = new TreeSet<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(link.getBody(), true);
		while (it.hasNext()) {
			for (Reference r : it.next().getReferencesFrom()) {
				if (r.getToAddress().isMemoryAddress() && (r.getReferenceType().isWrite() || r.getReferenceType().isData())
					&& currentProgram.getListing().getInstructionAt(r.getToAddress()) == null) {
					written.add(r.getToAddress().getOffset());
				}
			}
		}
		if (written.isEmpty() || written.contains(addrFirstTypeInfo)) {
			if (blockAt(addrFirstTypeInfo) == null) {
				printerr("FrostbiteTypeDumper: WARNING list head " + hex(addrFirstTypeInfo) + " is not in program memory");
			}
			return preListed;
		}
		if (written.size() == 1) {
			long cand = written.iterator().next();
			printerr("FrostbiteTypeDumper: WARNING the link function never touches the configured list head "
				+ hex(addrFirstTypeInfo) + "; using the address it does write, " + hex(cand)
				+ ". Update the profile if this is right.");
			addrFirstTypeInfo = cand;
			return countListInMemory();
		}
		printerr("FrostbiteTypeDumper: WARNING the link function never touches the configured list head "
			+ hex(addrFirstTypeInfo) + "; it references " + written.size() + " data addresses instead: "
			+ hexList(written));
		return preListed;
	}

	private static String hexList(Iterable<Long> l) {
		StringBuilder sb = new StringBuilder();
		for (long v : l) {
			sb.append(sb.length() > 0 ? ", " : "").append(hex(v));
		}
		return sb.toString();
	}

	// ------------------------------------------------------------------
	// Options
	// ------------------------------------------------------------------

	private File chooseOutputDir() throws Exception {
		String[] args = getScriptArgs();
		if (args != null && args.length > 0 && args[0].length() > 0) {
			File d = new File(args[0]);
			d.mkdirs();
			return d;
		}
		if (isRunningHeadless()) {
			File d = new File(System.getProperty("user.home"), "fb_type_dump");
			d.mkdirs();
			return d;
		}
		return askDirectory("Output folder for the type dump", "Select");
	}

	private boolean chooseAnnotate() throws Exception {
		String[] args = getScriptArgs();
		if (args != null && args.length > 1
			&& (args[1].equalsIgnoreCase("nolabels") || args[1].equalsIgnoreCase("noannotate"))) {
			return false;
		}
		if (isRunningHeadless()) {
			return true;
		}
		return askYesNo("FrostbiteTypeDumper",
			"Annotate this program?\n\n"
				+ "- data types in /fb, /fb/vtables and /fb/reflection, applied to every record\n"
				+ "- labels fb::TI_/TID_/FID_<Name>\n"
				+ "- fb::<Class>::getType / create / vftable, and virtual methods unique to a class\n\n"
				+ "Re-running replaces the script's previous annotation.");
	}

	// ------------------------------------------------------------------
	// Finding the static initializers
	// ------------------------------------------------------------------

	/**
	 * Preferred: the entries of the initializer table (ELF .init_array, or the MSVC CRT
	 * __xc_a..__xc_z table) that reach a registration primitive, in the order they run.
	 * Fallback: walk call references upward from the registration primitives and treat
	 * functions with no incoming call/jump references as roots (they are only reachable
	 * from data, i.e. the initializer table), ordered by the address of the pointer to them.
	 */
	private List<Function> findInitializerRoots() {
		FunctionManager fm = currentProgram.getFunctionManager();
		List<Long> initArray = readInitArray();

		Map<Function, Integer> depth = registrationCallGraph(
			initArray.isEmpty() ? MAX_CALL_DEPTH_SEARCH : MAX_CALL_DEPTH_INIT_ARRAY);

		Set<Long> excluded = new HashSet<>();
		excluded.add(fnAssignId);
		excluded.add(fnLink);
		excluded.addAll(helperCategory.keySet());

		if (!initArray.isEmpty()) {
			Set<Function> ordered = new LinkedHashSet<>();
			for (long p : initArray) {
				Function f = fm.getFunctionAt(toAddr(p));
				if (f != null && depth.containsKey(f) && !excluded.contains(p)) {
					ordered.add(f);
				}
			}
			if (!ordered.isEmpty()) {
				rootSource = initTableName + ": " + ordered.size() + " of " + initArray.size()
					+ " entries reach a registration primitive";
				return new ArrayList<>(ordered);
			}
		}

		// Order guessed roots by where the pointer to them lives: entries of the (unnamed)
		// initializer table then come out in table order. Roots with no data pointer go last.
		Map<Function, Long> order = new HashMap<>();
		ReferenceManager rm = currentProgram.getReferenceManager();
		for (Function f : depth.keySet()) {
			if (excluded.contains(f.getEntryPoint().getOffset()) || !codeCallers(f).isEmpty()) {
				continue;
			}
			long from = Long.MAX_VALUE;
			ReferenceIterator it = rm.getReferencesTo(f.getEntryPoint());
			while (it.hasNext()) {
				Reference r = it.next();
				if (r.getReferenceType().isData() && r.getFromAddress().isMemoryAddress()) {
					from = Math.min(from, r.getFromAddress().getOffset());
				}
			}
			order.put(f, from);
		}
		List<Function> roots = new ArrayList<>(order.keySet());
		roots.sort(Comparator.comparingLong((Function fn) -> order.get(fn))
			.thenComparingLong(fn -> fn.getEntryPoint().getOffset()));
		rootSource = initArray.isEmpty()
			? "no initializer table found: guessed from the call graph"
			: initTableName + " entries did not reach registration: guessed from the call graph";
		return roots;
	}

	private String initTableName = "init array";

	/** Functions that reach a registration primitive within maxDepth calls, with their depth. */
	private Map<Function, Integer> registrationCallGraph(int maxDepth) {
		FunctionManager fm = currentProgram.getFunctionManager();
		ReferenceManager rm = currentProgram.getReferenceManager();

		Map<Function, Integer> depth = new HashMap<>();
		Deque<Function> queue = new ArrayDeque<>();

		for (long s : new long[] { fnAssignId, fnLink }) {
			Function f = fm.getFunctionAt(toAddr(s));
			if (f != null && !depth.containsKey(f)) {
				depth.put(f, 0);
				queue.add(f);
			}
		}
		for (Object[] h : helpers) {
			Function f = fm.getFunctionAt(toAddr((Long) h[0]));
			if (f != null && !depth.containsKey(f)) {
				depth.put(f, 1);
				queue.add(f);
			}
		}
		// Code that touches the list head directly (inlined registration).
		ReferenceIterator headRefs = rm.getReferencesTo(toAddr(addrFirstTypeInfo));
		while (headRefs.hasNext()) {
			Reference r = headRefs.next();
			Function f = fm.getFunctionContaining(r.getFromAddress());
			if (f != null && !depth.containsKey(f)) {
				depth.put(f, 1);
				queue.add(f);
			}
		}

		while (!queue.isEmpty()) {
			Function f = queue.poll();
			int d = depth.get(f);
			if (d >= maxDepth) {
				continue;
			}
			for (Function caller : codeCallers(f)) {
				if (!depth.containsKey(caller)) {
					depth.put(caller, d + 1);
					queue.add(caller);
				}
			}
		}
		return depth;
	}

	/**
	 * Entries of the initializer table: ELF DT_INIT_ARRAY (".init_array" block or the
	 * __init_array_start/__init_array_end symbols), or for PE the CRT table (".CRT" block or
	 * the __xc_a/__xc_z symbols). Empty if none exists (Scylla dumps usually lose both the
	 * section names and the symbols).
	 */
	private List<Long> readInitArray() {
		long start = 0;
		long end = 0;
		String[][] candidates = isPe
			? new String[][] { { ".CRT", "__xc_a", "__xc_z" } }
			: new String[][] { { ".init_array", "__init_array_start", "__init_array_end" } };
		for (String[] c : candidates) {
			MemoryBlock b = mem.getBlock(c[0]);
			if (b != null && b.isInitialized()) {
				start = b.getStart().getOffset();
				end = b.getEnd().getOffset() + 1;
				initTableName = c[0];
				break;
			}
			Symbol s = firstGlobalSymbol(c[1]);
			Symbol e = firstGlobalSymbol(c[2]);
			if (s != null && e != null) {
				start = s.getAddress().getOffset();
				end = e.getAddress().getOffset();
				initTableName = c[1] + ".." + c[2];
				break;
			}
		}
		List<Long> out = new ArrayList<>();
		if (start == 0 || end <= start || end - start > 0x100000) {
			return out;
		}
		for (long a = start; a + 8 <= end; a += 8) {
			long p = readQ(a);
			if (p != 0 && p != -1L) {
				out.add(p);
			}
		}
		return out;
	}

	private Symbol firstGlobalSymbol(String name) {
		try {
			List<Symbol> l = currentProgram.getSymbolTable().getGlobalSymbols(name);
			return l.isEmpty() ? null : l.get(0);
		}
		catch (Exception e) {
			return null;
		}
	}

	private List<Function> codeCallers(Function f) {
		FunctionManager fm = currentProgram.getFunctionManager();
		ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
		Set<Function> out = new LinkedHashSet<>();
		while (it.hasNext()) {
			Reference r = it.next();
			RefType t = r.getReferenceType();
			if (!(t.isCall() || t.isJump())) {
				continue;
			}
			Function c = fm.getFunctionContaining(r.getFromAddress());
			if (c != null && !c.equals(f)) {
				out.add(c);
			}
		}
		return new ArrayList<>(out);
	}

	// ------------------------------------------------------------------
	// Emulation
	// ------------------------------------------------------------------

	private String emulateRoot(Function f) throws Exception {
		long sp = STACK_TOP - STACK_HEADROOM - 8; // 8 mod 16, as after a call
		writeQ(sp, SENTINEL_RET);
		emu.writeRegister("RSP", sp);
		// Every register starts at zero, so a root never sees the previous root's state.
		for (String r : RESET_REGS) {
			try {
				emu.writeRegister(r, 0);
			}
			catch (Exception e) {
				// register name not in this language
			}
		}
		setPC(f.getEntryPoint().getOffset());

		long steps = 0;
		int skips = 0;
		boolean checkHead = true;
		long expectedNext = -1;
		while (steps < MAX_STEPS_PER_ROOT) {
			if (monitor.isCancelled()) {
				return "cancelled";
			}
			long pc = emu.getExecutionAddress().getOffset();

			// Catch inline registrations: anything that changes the list head. Only
			// instructions with a STORE (and faked imports) can change it.
			if (checkHead) {
				long head = readQ(addrFirstTypeInfo);
				if (head != lastHead) {
					if (head >= MIN_VALID_ADDR) {
						recordListInsert(head, pc);
					}
					lastHead = head;
				}
				checkHead = false;
			}

			if (pc == SENTINEL_RET) {
				return "ok (" + steps + " steps" + (skips > 0 ? ", " + skips + " skipped" : "") + ")";
			}
			if (pc == fnAssignId || pc == fnLink) {
				recordRegistration(pc);
			}
			Integer sizeArg = allocatorSizeArg.get(pc);
			if (sizeArg != null) {
				long size = reg(argRegs[Math.min(sizeArg, 2)]);
				fakeReturn(bumpAlloc(size));
				steps++;
				expectedNext = -1;
				continue;
			}

			// Import thunks are only ever entered by a branch, never by falling through.
			if (pc != expectedNext) {
				String imp = importThunkName(pc);
				if (imp != null) {
					fakeImportNamed(imp);
					steps++;
					checkHead = true;
					expectedNext = -1;
					continue;
				}
			}
			MemoryBlock b = blockAt(pc);
			if (b == null) {
				// pc < MIN_VALID_ADDR: virtual call through a null/zero vtable.
				// Otherwise: a call into a module that is not in this image (resolved IAT
				// in a PC dump). Both are treated as "returned 0" if a return address is on top.
				if (skips < MAX_SKIPS_PER_ROOT && (pc < MIN_VALID_ADDR || returnsToCode())) {
					skips++;
					totalSkips++;
					fakeReturn(0);
					steps++;
					expectedNext = -1;
					continue;
				}
				return "abort: jumped to unmapped " + hex(pc) + " after " + steps + " steps";
			}
			Address pcAddr = toAddr(pc);
			if (!b.isExecute() || !b.isInitialized() || b.getName().equals("EXTERNAL")) {
				// Imported function (EXTERNAL block) or non-code: emulate the few
				// libc helpers that matter, return 0 for everything else.
				fakeImport(pcAddr);
				steps++;
				checkHead = true;
				expectedNext = -1;
				continue;
			}

			int info = insInfo(pc);
			boolean stepped;
			String err = null;
			try {
				stepped = emu.step(monitor);
				if (!stepped) {
					err = emu.getLastError();
				}
			}
			catch (Exception e) {
				stepped = false;
				err = e.getClass().getSimpleName() + ": " + e.getMessage();
			}
			if (!stepped) {
				// Skip instructions the emulator cannot model and keep going.
				if (info >= 0 && skips < MAX_SKIPS_PER_ROOT) {
					skips++;
					totalSkips++;
					setPC(pc + (info >> 1));
					steps++;
					expectedNext = pc + (info >> 1);
					continue;
				}
				return "abort: " + err + " at " + hex(pc) + " after " + steps + " steps, " + skips + " skips";
			}
			checkHead = info < 0 || (info & 1) != 0;
			expectedNext = info < 0 ? -1 : pc + (info >> 1);
			steps++;
		}
		return "abort: step budget exhausted";
	}

	/** Cached (length << 1 | hasStore) for the instruction at pc, -1 if the listing has none. */
	private int insInfo(long pc) {
		Integer v = insInfoCache.get(pc);
		if (v != null) {
			return v;
		}
		int r = -1;
		Instruction ins = currentProgram.getListing().getInstructionAt(toAddr(pc));
		if (ins != null) {
			boolean store = false;
			for (PcodeOp op : ins.getPcode()) {
				if (op.getOpcode() == PcodeOp.STORE) {
					store = true;
					break;
				}
			}
			r = (ins.getLength() << 1) | (store ? 1 : 0);
		}
		insInfoCache.put(pc, r);
		return r;
	}

	/** Name of the external function if pc is a thunk to one, else null. Cached. */
	private String importThunkName(long pc) {
		String cached = importThunkCache.get(pc);
		if (cached == null) {
			cached = "";
			Function here = currentProgram.getFunctionManager().getFunctionAt(toAddr(pc));
			if (here != null && here.isThunk()) {
				Function target = here.getThunkedFunction(true);
				if (target != null && target.isExternal()) {
					cached = target.getName();
				}
			}
			importThunkCache.put(pc, cached);
		}
		return cached.isEmpty() ? null : cached;
	}

	/** True if the qword on top of the stack is an address in executable program memory. */
	private boolean returnsToCode() {
		MemoryBlock rb = blockAt(readQ(reg("RSP")));
		return rb != null && rb.isExecute() && rb.isInitialized();
	}

	/** Number of entries reachable from the list head as the image stands (before emulation). */
	private int countListInMemory() {
		Set<Long> seen = new HashSet<>();
		long ti = readQ(addrFirstTypeInfo);
		while (ti >= MIN_VALID_ADDR && !seen.contains(ti) && seen.size() < 200_000 && looksLikeTypeInfo(ti)) {
			seen.add(ti);
			ti = readQ(ti + 8);
		}
		return seen.size();
	}

	private void recordRegistration(long pc) {
		long ti = reg(argRegs[0]);
		if (ti < MIN_VALID_ADDR) {
			return;
		}
		long ret = readQ(reg("RSP"));
		Function caller = currentProgram.getFunctionManager().getFunctionContaining(toAddr(ret));
		long helper = caller == null ? 0 : caller.getEntryPoint().getOffset();

		Reg r = regs.get(ti);
		if (r == null) {
			r = new Reg();
			r.ti = ti;
			regs.put(ti, r);
		}
		if (pc == fnAssignId) {
			r.data = reg(argRegs[1]);
		}
		// Prefer a known helper over an unknown one.
		if (r.helper == 0 || (!helperCategory.containsKey(r.helper) && helperCategory.containsKey(helper))) {
			r.helper = helper;
		}
	}

	private void recordListInsert(long ti, long pc) {
		Reg r = regs.get(ti);
		if (r == null) {
			r = new Reg();
			r.ti = ti;
			regs.put(ti, r);
		}
		if (r.data == 0) {
			r.data = readQ(ti);
		}
		Function f = currentProgram.getFunctionManager().getFunctionContaining(toAddr(pc));
		long helper = f == null ? 0 : f.getEntryPoint().getOffset();
		if (r.helper == 0 || (!helperCategory.containsKey(r.helper) && helperCategory.containsKey(helper))) {
			r.helper = helper;
		}
	}

	private void fakeImport(Address pcAddr) {
		Function f = currentProgram.getFunctionManager().getFunctionAt(pcAddr);
		fakeImportNamed(f == null ? "" : f.getName());
	}

	private void fakeImportNamed(String name) {
		long a0 = reg(argRegs[0]);
		long a1 = reg(argRegs[1]);
		long a2 = reg(argRegs[2]);
		long result = 0;
		switch (name) {
			case "memset": {
				byte[] buf = new byte[clampLen(a2)];
				Arrays.fill(buf, (byte) a1);
				writeBytes(a0, buf);
				result = a0;
				break;
			}
			case "memcpy":
			case "memmove":
				writeBytes(a0, readBytes(a1, clampLen(a2)));
				result = a0;
				break;
			case "strlen":
				result = strlenRaw(a0);
				break;
			case "malloc":
			case "operator.new":
			case "operator_new":
			case "operator.new[]":
			case "_Znwm":
			case "_Znam":
				result = bumpAlloc(a0);
				break;
			case "calloc":
				result = bumpAlloc(a0 * a1);
				break;
			default:
				break;
		}
		fakeReturn(result);
	}

	/** Length argument as an unsigned value clamped to [0, MAX_FAKE_COPY]. */
	private static int clampLen(long n) {
		return Long.compareUnsigned(n, MAX_FAKE_COPY) > 0 ? MAX_FAKE_COPY : (int) n;
	}

	/** Bytes before the first NUL (any byte value counts), capped at 64 KiB. */
	private long strlenRaw(long a) {
		final int cap = 1 << 16;
		for (int off = 0; off < cap; off += 64) {
			byte[] c = readBytes(a + off, 64);
			for (int i = 0; i < c.length; i++) {
				if (c[i] == 0) {
					return off + i;
				}
			}
		}
		return cap;
	}

	private void fakeReturn(long value) {
		long sp = reg("RSP");
		long ret = readQ(sp);
		emu.writeRegister("RSP", sp + 8);
		emu.writeRegister("RAX", value);
		setPC(ret);
	}

	/** Redirects execution. Writes both the RIP register and the emulator's fetch address. */
	private void setPC(long pc) {
		emu.writeRegister("RIP", pc);
		emu.getEmulator().setExecuteAddress(pc);
	}

	private long bumpAlloc(long size) {
		if (size <= 0 || size > (64L << 20)) {
			size = 0x100;
		}
		long p = (heapCursor + 0x7f) & ~0x7fL;
		if (p + size > FAKE_HEAP_END) {
			if (!heapWrapped) {
				println("FrostbiteTypeDumper: WARNING fake heap exhausted, wrapping: heap-allocated types "
					+ "created before this point may be overwritten");
				rootLog.add("WARNING: fake heap wrapped");
			}
			heapWrapped = true;
			p = FAKE_HEAP_BASE;
		}
		heapCursor = p + size;
		return p;
	}

	// ------------------------------------------------------------------
	// Building the type table
	// ------------------------------------------------------------------

	private void collectTypes() {
		// Authoritative: the engine's own linked list.
		Set<Long> seen = new HashSet<>();
		long ti = readQ(addrFirstTypeInfo);
		int guard = 0;
		while (ti >= MIN_VALID_ADDR && !seen.contains(ti) && guard++ < 500_000 && isReadable(ti)) {
			seen.add(ti);
			addType(ti);
			ti = readQ(ti + 8);
		}
		println("FrostbiteTypeDumper: " + seen.size() + " types on the global list, "
			+ regs.size() + " registrations intercepted");
		// Anything intercepted but not on the list (e.g. registered then unlinked).
		for (Long r : new TreeSet<>(regs.keySet())) {
			if (r >= MIN_VALID_ADDR && !types.containsKey(r) && looksLikeTypeInfo(r)) {
				addType(r);
			}
		}
	}

	private void addType(long ti) {
		addType(ti, readQ(ti));
	}

	private void addType(long ti, long dataOverride) {
		TypeRec t = new TypeRec();
		t.ti = ti;
		t.data = dataOverride;
		Reg r = regs.get(ti);
		if (r != null) {
			t.helper = r.helper;
			if (t.data == 0 && r.data != 0) {
				t.data = r.data;
			}
		}
		if (t.data != 0 && isReadable(t.data)) {
			long namePtr = readQ(t.data);
			String n = readCString(namePtr);
			t.name = n == null ? "" : n;
			t.flags = readU16(t.data + 8);
			t.size = readU16(t.data + 0xA);
			t.alignment = readU8(t.data + TID_ALIGN_OFF);
			t.module = readModuleName(readQ(t.data + TID_MODULE_OFF));
		}
		if (t.name.isEmpty()) {
			t.name = "Unnamed_" + Long.toHexString(ti);
		}
		primIndex.remove(ti);
		types.put(ti, t);
	}

	/**
	 * Name of a ModuleInfo. Layout not confirmed: the name is taken from the first qword
	 * (ModuleInfo { const char* name; ... }), or from the pointer itself if it is a string.
	 */
	private String readModuleName(long mod) {
		if (mod < MIN_VALID_ADDR || !isReadable(mod)) {
			return "";
		}
		Long key = mod;
		String cached = moduleNames.get(key);
		if (cached != null) {
			return cached;
		}
		String n = readCString(readQ(mod));
		if (n == null) {
			n = readCString(mod);
		}
		if (n == null || n.length() > 64) {
			n = "";
		}
		moduleNames.put(key, n);
		return n;
	}

	private final Map<Long, String> moduleNames = new HashMap<>();

	/** Named primitives with a static record, by TypeInfo address (neighbours for interpolation). */
	private final TreeMap<Long, TypeRec> primIndex = new TreeMap<>();

	private void indexPrimitive(TypeRec t) {
		primIndex.remove(t.ti);
		if (t.category.equals("Primitive") && t.data != 0 && !t.name.startsWith("Unnamed_")) {
			primIndex.put(t.ti, t);
		}
	}

	/** True if ti points at something shaped like a TypeInfo (data pointer -> named record). */
	private boolean looksLikeTypeInfo(long ti) {
		if (ti < MIN_VALID_ADDR || !isReadable(ti)) {
			return false;
		}
		long data = readQ(ti);
		if (data < MIN_VALID_ADDR || !isReadable(data)) {
			return false;
		}
		long namePtr = readQ(data);
		// Array TypeInfoData names are often not printable strings; accept any readable pointer.
		return namePtr == 0 || isReadable(namePtr);
	}

	/** Category, super, element type and fields, using the confirmed fixed layout. */
	private void processType(TypeRec t) {
		String byHelper = helperCategory.get(t.helper);
		t.helperCategory = byHelper == null ? "" : byHelper;
		// The flags encode the category directly (confirmed for every PC type); the helper
		// that registered the type is only a fallback for records without usable flags.
		String cat = t.data != 0 ? categoryFromFlags(t.flags) : null;
		if (cat == null) {
			cat = byHelper != null ? byHelper : (PRIMITIVE_C.containsKey(t.name) ? "Primitive" : "Unknown");
		}
		t.category = cat;
		indexPrimitive(t);
		t.fields.clear();
		t.fieldsPtr = 0;
		t.fieldsPtrOff = -1;
		if (isReadable(t.ti)) {
			t.runtimeId = readU16(t.ti + TI_RUNTIME_ID_OFF);
			t.unk12 = readU16(t.ti + TI_RUNTIME_ID_OFF + 2);
			if (cat.equals("Class")) {
				t.defaultInstance = readQ(t.ti + CI_DEFAULT_INSTANCE_OFF);
				t.classId = readU16(t.ti + CI_CLASS_ID_OFF);
				t.lastSubClassId = readU16(t.ti + CI_CLASS_ID_OFF + 2);
				t.firstDerived = readQ(t.ti + CI_FIRST_DERIVED_OFF);
				t.nextSibling = readQ(t.ti + CI_FIRST_DERIVED_OFF + 8);
				t.slot50 = readQ(t.ti + CI_SLOT50_OFF);
				t.slot58 = readQ(t.ti + CI_SLOT50_OFF + 8);
			}
		}
		if (t.data == 0) {
			return;
		}
		if (cat.equals("ValueType")) {
			long p = readQ(t.data + VALUETYPE_INSTANCE_OFF);
			t.valueTypeUnk20 = p >= MIN_VALID_ADDR && isReadable(p) ? p : 0;
			t.staticInstance = 0; // set only once its vtable proves it (messages)
		}

		if (cat.equals("Class")) {
			long sup = readQ(t.ti + CLASS_RUNTIME_SUPER_OFF);
			if (!looksLikeTypeInfo(sup)) {
				sup = readQ(t.data + CLASS_DATA_SUPER_OFF);
			}
			t.superTi = (sup != t.ti && looksLikeTypeInfo(sup)) ? sup : 0;
		}
		if (cat.equals("Array") || cat.equals("FixedArray")) {
			long el = readQ(t.data + ARRAY_ELEMENT_OFF);
			t.elementTi = (el != t.ti && looksLikeTypeInfo(el)) ? el : 0;
		}

		int fieldsOff = -1;
		if (cat.equals("Class")) {
			fieldsOff = CLASS_FIELDS_PTR_OFF;
		}
		else if (cat.equals("ValueType")) {
			fieldsOff = VALUETYPE_FIELDS_PTR_OFF;
		}
		else if (cat.equals("Enum")) {
			fieldsOff = ENUM_FIELDS_PTR_OFF;
		}
		if (fieldsOff < 0) {
			return;
		}
		int count = readU16(t.data + TID_FIELD_COUNT_OFF);
		long p = readQ(t.data + fieldsOff);
		if (count == 0 || count > 2048 || p < MIN_VALID_ADDR || !isReadable(p)) {
			return;
		}
		t.fieldsPtr = p;
		t.fieldsPtrOff = fieldsOff;
		for (int i = 0; i < count; i++) {
			long e = p + (long) i * FIELD_STRIDE;
			Field f = new Field();
			f.name = readCString(readQ(e));
			if (f.name == null) {
				f.name = "field_" + i;
			}
			f.flags = readU16(e + 8);
			f.offset = readU16(e + 0xA);
			f.raw0C = readU32(e + 0xC);
			f.typeTi = readQ(e + 0x10);
			t.fields.add(f);
		}
	}

	/** Adds types that are referenced (super/element/field type) but were never seen on the list. */
	private void discoverReferencedTypes() {
		int added = 0;
		for (int round = 0; round < 16; round++) {
			// Sorted so the result does not depend on hash order.
			Set<Long> refs = new TreeSet<>();
			for (TypeRec t : types.values()) {
				refs.add(t.superTi);
				refs.add(t.elementTi);
				if (!t.category.equals("Enum")) {
					for (Field f : t.fields) {
						refs.add(f.typeTi);
					}
				}
			}
			int fresh = 0;
			for (long r : refs) {
				if (r < MIN_VALID_ADDR || types.containsKey(r)) {
					continue;
				}
				// Each new type is categorised immediately, so later references in this
				// same round can already use it (array rule, interpolation neighbours).
				if (looksLikeTypeInfo(r)) {
					addType(r);
					processType(types.get(r));
					fresh++;
					continue;
				}
				TypeRec inferred = inferUninitialisedType(r);
				if (inferred != null) {
					if (!inferred.note.startsWith("inferred array")) {
						processType(inferred);
					}
					fresh++;
				}
			}
			if (fresh == 0) {
				break;
			}
			added += fresh;
		}
		println("FrostbiteTypeDumper: discovered " + added + " referenced types not on the list (primitives, value types)");
	}

	/**
	 * Recovers a referenced TypeInfo whose registration was lost in emulation (e.g. a skipped
	 * instruction). Two checked rules:
	 *  1. Primitive TypeInfos are laid out at a fixed stride, as are their static records.
	 *     Interpolate the record address from the nearest known neighbours on both sides and
	 *     accept it only if the record has a readable name and a primitive-looking flags value.
	 *  2. PS4 only (primitiveArrayOff): each primitive object also holds Array<primitive>
	 *     at +0x20. On PC that address is the next primitive, so the rule is off there.
	 */
	private TypeRec inferUninitialisedType(long ti) {
		// Rule 2: Array<primitive> at primitive + primitiveArrayOff.
		TypeRec elem = primitiveArrayOff > 0 ? types.get(ti - primitiveArrayOff) : null;
		if (elem != null && elem.category.equals("Primitive")) {
			TypeRec t = new TypeRec();
			t.ti = ti;
			t.category = "Array";
			t.elementTi = elem.ti;
			t.size = 8;
			t.name = "";
			t.note = "inferred array (primitive+0x20)";
			types.put(ti, t);
			return t;
		}
		// Rule 1: interpolate between primitive neighbours.
		long data = interpolatePrimitiveData(ti);
		if (data == 0) {
			return null;
		}
		addType(ti, data);
		TypeRec t = types.get(ti);
		t.helper = 0;
		t.note = "inferred (record interpolated from neighbouring primitives)";
		return t;
	}

	/**
	 * Primitive TypeInfos and their static records are each laid out at a fixed stride, in the
	 * same order (PS4: 0x40 / 0xA0; the strides are not assumed, only the linearity). Returns
	 * the interpolated static record for ti, or 0 if the neighbours do not line up, the record
	 * does not look like a named primitive, or another type already owns that record.
	 */
	private long interpolatePrimitiveData(long ti) {
		Map.Entry<Long, TypeRec> lo = primIndex.lowerEntry(ti);
		Map.Entry<Long, TypeRec> hi = primIndex.higherEntry(ti);
		if (lo == null || hi == null) {
			return 0;
		}
		TypeRec below = lo.getValue();
		TypeRec above = hi.getValue();
		long tiSpan = above.ti - below.ti;
		long dataSpan = above.data - below.data;
		if (tiSpan <= 0 || dataSpan <= 0 || ((ti - below.ti) * dataSpan) % tiSpan != 0) {
			return 0;
		}
		long data = below.data + (ti - below.ti) * dataSpan / tiSpan;
		String name = readCString(readQ(data));
		int flags = readU16(data + 8);
		if (name == null || !"Primitive".equals(categoryFromFlags(flags))) {
			return 0;
		}
		if (claimedData().contains(data)) {
			return 0;
		}
		return data;
	}

	private Set<Long> claimedDataCache;
	private int claimedDataSize = -1;

	/** Static records already owned by a type (rebuilt when the type table grows). */
	private Set<Long> claimedData() {
		if (claimedDataCache == null || claimedDataSize != types.size()) {
			claimedDataCache = new HashSet<>();
			for (TypeRec t : types.values()) {
				if (t.data != 0) {
					claimedDataCache.add(t.data);
				}
			}
			claimedDataSize = types.size();
		}
		return claimedDataCache;
	}

	/**
	 * A primitive slot can be overwritten during emulation (seen for CString). Any nameless
	 * non-array type that sits inside the primitive table is re-read from its interpolated record.
	 */
	private void repairPrimitiveTable() {
		int repaired = 0;
		for (TypeRec t : new ArrayList<>(types.values())) {
			if (!t.name.startsWith("Unnamed_") || t.category.equals("Array")) {
				continue;
			}
			long data = interpolatePrimitiveData(t.ti);
			if (data == 0) {
				continue;
			}
			t.data = data;
			t.name = readCString(readQ(data));
			t.flags = readU16(data + 8);
			t.size = readU16(data + 0xA);
			t.alignment = readU8(data + TID_ALIGN_OFF);
			t.module = readModuleName(readQ(data + TID_MODULE_OFF));
			t.helper = 0;
			t.note = "repaired (emulated slot was overwritten; record interpolated from neighbouring primitives)";
			processType(t);
			claimedDataCache = null;
			repaired++;
		}
		println("FrostbiteTypeDumper: repaired " + repaired + " primitive slots");
	}

	/** Type code (TypeEnum) stored in bits 4..8 of TypeInfoData flags. Not valid for field flags. */
	static int kind(int flags) {
		return (flags >> 4) & 0x1f;
	}

	/** Category from a TypeInfoData's type code, or null for codes outside the known table. */
	static String categoryFromFlags(int flags) {
		switch (kind(flags)) {
			case 2:
				return "ValueType";
			case 3:
				return "Class";
			case 4:
				return "Array";
			case 5:
				return "FixedArray";
			case 8:
				return "Enum";
			case 24:
				return "Function";
			case 26:
				return "BoxedValueRef";
			case 27:
				return "Interface";
			case 28:
				return "Delegate";
			default:
				return KIND_PRIMITIVE.containsKey(kind(flags)) ? "Primitive" : null;
		}
	}

	/**
	 * Layout checks written to the calibration file:
	 *  - fieldCount against the room before the next known record (a field table that runs
	 *    into another TypeInfo, TypeInfoData or field table means a wrong count or pointer);
	 *  - the category from the flags against the category the registration helper implies;
	 *  - the size of each primitive against its C mapping.
	 */
	private void crossCheck() {
		TreeSet<Long> starts = knownRecordStarts();
		Map<String, int[]> stats = new TreeMap<>();
		List<String> overruns = new ArrayList<>();
		for (TypeRec t : types.values()) {
			if (t.fieldsPtr == 0 || t.fields.isEmpty()) {
				continue;
			}
			Long next = starts.higher(t.fieldsPtr);
			long room = next == null ? Long.MAX_VALUE : (next - t.fieldsPtr) / FIELD_STRIDE;
			int[] st = stats.computeIfAbsent(t.category, k -> new int[2]);
			st[0]++;
			if (t.fields.size() > room) {
				st[1]++;
				if (overruns.size() < 10) {
					overruns.add(t.name + " (" + t.fields.size() + " fields, room for " + room + ")");
				}
			}
		}
		for (Map.Entry<String, int[]> e : stats.entrySet()) {
			int[] st = e.getValue();
			calibrationNotes.put(e.getKey() + " field tables", st[0] + " checked, " + st[1]
				+ " run into the next known record (suspicious)");
		}
		if (!overruns.isEmpty()) {
			calibrationNotes.put("field table overruns (first 10)", overruns.toString());
		}

		checkRuntimeLayout();

		Map<String, Integer> disagree = new TreeMap<>();
		for (TypeRec t : types.values()) {
			if (!t.helperCategory.isEmpty() && !t.helperCategory.equals(t.category)) {
				disagree.merge("helper says " + t.helperCategory + ", flags say " + t.category, 1, Integer::sum);
			}
		}
		calibrationNotes.put("category: helper vs flags", disagree.isEmpty() ? "all agree" : disagree.toString());

		List<String> sizeMismatch = new ArrayList<>();
		for (TypeRec t : types.values()) {
			String[] c = PRIMITIVE_C.get(t.name);
			if (t.category.equals("Primitive") && c != null && t.size > 0 && t.size != Integer.parseInt(c[1])) {
				sizeMismatch.add(t.name + " is 0x" + Integer.toHexString(t.size) + ", not " + c[1]);
			}
		}
		calibrationNotes.put("primitive sizes", sizeMismatch.isEmpty() ? "all match the C mapping"
			: "emitted as byte blobs: " + sizeMismatch);
		for (String s : sizeMismatch) {
			println("FrostbiteTypeDumper: primitive " + s + " bytes: emitted as an opaque blob");
		}
	}

	private Map<String, String> calibrationNotes = new TreeMap<>();

	/** True once the ClassInfo classId/lastSubClassId/derived/sibling fields passed validation. */
	private boolean classTreeValid;
	/** True if the runtime ids at TypeInfo+0x10 are unique. */
	private boolean runtimeIdsValid;

	/**
	 * Validates the runtime ClassInfo layout against the super-class tree from the static
	 * records, which is known to be right:
	 *  - firstDerived is null for a leaf, else a class whose super is this class;
	 *  - nextSibling is null or a class with the same super;
	 *  - lastSubClassId - classId equals the number of (transitive) subclasses;
	 *  - classIds are unique.
	 * The fields are only output when every check holds for at least 95% of the classes.
	 */
	private void checkRuntimeLayout() {
		Map<Long, List<TypeRec>> children = new HashMap<>();
		List<TypeRec> classes = new ArrayList<>();
		for (TypeRec t : types.values()) {
			if (t.category.equals("Class") && t.classId >= 0) {
				classes.add(t);
				if (t.superTi != 0) {
					children.computeIfAbsent(t.superTi, k -> new ArrayList<>()).add(t);
				}
			}
		}
		if (classes.isEmpty()) {
			return;
		}
		int derivedOk = 0;
		int siblingOk = 0;
		int rangeOk = 0;
		Set<Integer> ids = new HashSet<>();
		int idsUnique = 0;
		List<String> rangeBad = new ArrayList<>();
		for (TypeRec t : classes) {
			List<TypeRec> kids = children.getOrDefault(t.ti, List.of());
			TypeRec fd = types.get(t.firstDerived);
			if ((t.firstDerived == 0 && kids.isEmpty()) || (fd != null && fd.superTi == t.ti)) {
				derivedOk++;
			}
			TypeRec ns = types.get(t.nextSibling);
			if (t.nextSibling == 0 || (ns != null && ns.superTi == t.superTi)) {
				siblingOk++;
			}
			int desc = descendantCount(t, children);
			if (t.lastSubClassId - t.classId == desc) {
				rangeOk++;
			}
			else if (rangeBad.size() < 5) {
				rangeBad.add(t.name + " " + t.classId + ".." + t.lastSubClassId + " vs " + desc + " subclasses");
			}
			if (ids.add(t.classId)) {
				idsUnique++;
			}
		}
		int n = classes.size();
		classTreeValid = derivedOk >= 0.95 * n && siblingOk >= 0.95 * n && rangeOk >= 0.95 * n && idsUnique >= 0.95 * n;
		calibrationNotes.put("ClassInfo layout (+0x28 classId, +0x2A lastSubClassId, +0x30 firstDerived, +0x38 nextSibling)",
			"firstDerived ok " + derivedOk + "/" + n + ", nextSibling ok " + siblingOk + "/" + n
				+ ", lastSubClassId-classId == subclass count " + rangeOk + "/" + n + ", unique classIds " + idsUnique + "/" + n
				+ (rangeBad.isEmpty() ? "" : ", e.g. " + rangeBad) + " -> " + (classTreeValid ? "USED" : "NOT used"));

		Set<Integer> rids = new HashSet<>();
		int withId = 0;
		int unique = 0;
		for (TypeRec t : types.values()) {
			if (t.runtimeId > 0) {
				withId++;
				unique += rids.add(t.runtimeId) ? 1 : 0;
			}
		}
		runtimeIdsValid = withId > 0 && unique >= 0.99 * types.size();
		calibrationNotes.put("runtime id (TypeInfo+0x10 u16)", unique + " unique of " + types.size() + " types -> "
			+ (runtimeIdsValid ? "USED" : "NOT used"));
	}

	private static int descendantCount(TypeRec root, Map<Long, List<TypeRec>> children) {
		int n = 0;
		Deque<TypeRec> stack = new ArrayDeque<>(children.getOrDefault(root.ti, List.of()));
		Set<Long> seen = new HashSet<>();
		while (!stack.isEmpty()) {
			TypeRec c = stack.pop();
			if (!seen.add(c.ti)) {
				continue;
			}
			n++;
			stack.addAll(children.getOrDefault(c.ti, List.of()));
		}
		return n;
	}

	private TreeSet<Long> knownStartsCache;

	/** Start addresses of every known TypeInfo, TypeInfoData and field table. */
	private TreeSet<Long> knownRecordStarts() {
		if (knownStartsCache == null) {
			knownStartsCache = new TreeSet<>();
			for (TypeRec t : types.values()) {
				knownStartsCache.add(t.ti);
				if (t.data != 0) {
					knownStartsCache.add(t.data);
				}
				if (t.fieldsPtr != 0) {
					knownStartsCache.add(t.fieldsPtr);
				}
			}
		}
		return knownStartsCache;
	}

	/**
	 * Gives every type a unique C/Ghidra identifier. Names that collapse to the same identifier
	 * (or the same name registered by two modules) get _2, _3... suffixes; C++ keywords get "_".
	 */
	private void assignIdents() {
		Set<String> used = new HashSet<>();
		for (TypeRec t : sortedTypes()) {
			String base = cIdent(typeName(t.ti));
			if (RESERVED_IDENTS.contains(base)) {
				base += "_";
			}
			String id = base;
			if (used.contains(id) && !t.module.isEmpty()) {
				id = base + "_" + cIdent(t.module); // same name in two modules
			}
			int n = 2;
			String stem = id;
			while (used.contains(id)) {
				id = stem + "_" + (n++);
			}
			used.add(id);
			t.ident = id;
		}
	}

	// ------------------------------------------------------------------
	// getType / create / vtables
	// ------------------------------------------------------------------

	/**
	 * Every reflected Frostbite class overrides a virtual getType() that only returns its
	 * TypeInfo. Three steps:
	 *  1. The accessors are found by byte pattern over code, independent of function
	 *     boundaries: in a Scylla dump most getType bodies are only reachable through vtables,
	 *     so Ghidra never made functions for them.
	 *  2. Vtables come from the create functions (ClassInfoData+0x28). create runs the
	 *     constructors, which store each vtable in turn (base first, most derived last). A
	 *     stored vtable whose getType slot holds type X's accessor is X's vtable, with no
	 *     guessing. The slot is a profile constant (PC 0); if unknown it is voted on here.
	 *  3. Classes without a create function (runtime classes) fall back to a data scan for
	 *     pointers to their accessor at the getType slot. Without RTTI the vtables sit back to
	 *     back, so the old "walk back to the RTTI entry" index is only used as a last resort.
	 */
	private void findReflectionMethods() throws CancelledException {
		Map<Long, TypeRec> accessorOwner = scanTypeAccessors();

		Map<TypeRec, List<Long>> createRefs = new LinkedHashMap<>();
		int notDisassembled = 0;
		for (TypeRec t : types.values()) {
			if (!t.category.equals("Class") || t.data == 0) {
				continue;
			}
			long c = readQ(t.data + CLASS_CREATE_FN_OFF);
			if (!isCodePointer(c)) {
				continue;
			}
			t.createFn = c;
			if (currentProgram.getListing().getInstructionAt(toAddr(c)) == null) {
				notDisassembled++;
				continue;
			}
			if (monitor.isCancelled()) {
				throw new CancelledException();
			}
			createRefs.put(t, dataRefsFromCode(c, 3));
		}

		String slotSource;
		if (profileGetTypeSlot >= 0) {
			getTypeVtableIndex = profileGetTypeSlot;
			slotSource = "profile";
		}
		else {
			getTypeVtableIndex = voteGetTypeSlot(createRefs, accessorOwner);
			slotSource = getTypeVtableIndex >= 0 ? "voted from create functions" : "not found from create functions";
		}

		int confirmed = 0;
		int mismatched = 0;
		if (getTypeVtableIndex >= 0) {
			for (Map.Entry<TypeRec, List<Long>> e : createRefs.entrySet()) {
				boolean own = false;
				for (long vt : e.getValue()) {
					long fn = progQ(vt + 8L * getTypeVtableIndex);
					TypeRec owner = accessorOwner.get(fn);
					if (owner == null || !canHaveVtable(owner)) {
						continue;
					}
					// Base constructors confirm the base classes' vtables as well.
					confirmVftable(owner, vt, fn);
					own |= owner == e.getKey();
				}
				if (own) {
					confirmed++;
				}
				else {
					mismatched++;
				}
			}
		}

		// Messages (value types with a vtable) have a static instance at ValueTypeInfoData+0x20
		// whose first qword is the message's vtable: the same proof as a create function.
		int fromInstance = 0;
		if (getTypeVtableIndex >= 0) {
			for (TypeRec t : types.values()) {
				if (t.valueTypeUnk20 == 0 || !canHaveVtable(t)) {
					continue;
				}
				long vt = readQ(t.valueTypeUnk20);
				long fn = progQ(vt + 8L * getTypeVtableIndex);
				if (vt != 0 && accessorOwner.get(fn) == t) {
					t.staticInstance = t.valueTypeUnk20;
					confirmVftable(t, vt, fn);
					fromInstance++;
				}
			}
		}

		int fromClassInfo = findClassInfoVtablesAndCreators(accessorOwner);

		int histogramSlot = findVtablesByScan(accessorOwner);
		if (getTypeVtableIndex < 0 && histogramSlot >= 0) {
			getTypeVtableIndex = histogramSlot;
			slotSource = "slot histogram (walk back to RTTI entry)";
		}

		int getTypes = 0;
		int vtables = 0;
		int creates = 0;
		for (TypeRec t : types.values()) {
			getTypes += t.getTypeFn != 0 ? 1 : 0;
			vtables += t.vftables.isEmpty() ? 0 : 1;
			creates += t.createFn != 0 ? 1 : 0;
		}
		String summary = accessorOwner.size() + " TypeInfo accessors, " + getTypes + " virtual getType(), "
			+ vtables + " types with a vtable, getType slot " + getTypeVtableIndex + " (" + slotSource + "), "
			+ creates + " create functions: " + confirmed + " stored their own vtable, " + mismatched
			+ " did not (vtable found by the data scan instead), " + notDisassembled + " not disassembled; "
			+ fromInstance + " message vtables confirmed by their static instance, " + fromClassInfo
			+ " vtables confirmed by ClassInfo+0x50";
		calibrationNotes.put("getType / vtables", summary);
		println("FrostbiteTypeDumper: " + summary);
	}

	/**
	 * ClassInfo+0x50/+0x58. A pointer whose getType slot holds this class's accessor is its
	 * vtable (data classes). A pointer to an object shaped like a creator (vtable of code,
	 * a class TypeInfo at +0x18, a small realm at +0x24 matching the slot) links a runtime
	 * class to the data class it is created from. Returns the number of vtables confirmed.
	 */
	private int findClassInfoVtablesAndCreators(Map<Long, TypeRec> accessorOwner) {
		int vtables = 0;
		int creators = 0;
		int named = 0;
		int shortClassInfos = 0;
		for (TypeRec t : types.values()) {
			if (!t.category.equals("Class")) {
				continue;
			}
			// 0x50-byte ClassInfos exist (no +0x50/+0x58 slots): there the slots would be
			// the next record's first qwords.
			long room = recordRoom(t.ti);
			if (room < CLASSINFO_EX_SIZE) {
				t.slot58 = 0; // a few ClassInfos have room for +0x50 only
				if (room < CLASSINFO_SIZE + 8) {
					t.slot50 = 0;
					shortClassInfos++;
					continue;
				}
			}
			long[] slots = { t.slot50, t.slot58 };
			for (int realmSlot = 0; realmSlot < 2; realmSlot++) {
				long p = slots[realmSlot];
				if (p < MIN_VALID_ADDR || !isReadable(p)) {
					continue;
				}
				if (realmSlot == 0 && getTypeVtableIndex >= 0 && canHaveVtable(t)) {
					long fn = progQ(p + 8L * getTypeVtableIndex);
					if (accessorOwner.get(fn) == t) {
						confirmVftable(t, p, fn);
						vtables++;
						continue;
					}
				}
				long cvt = readQ(p);
				TypeRec data = types.get(readQ(p + CREATOR_DATA_TI_OFF));
				long realm = readU32(p + CREATOR_REALM_OFF);
				if (cvt != 0 && isCodePointer(progQ(cvt)) && data != null && data.category.equals("Class")
					&& data != t && realm == realmSlot) {
					t.creators.add(new long[] { p, realm, data.ti });
					if (!data.createdBy.contains(t.ti)) {
						data.createdBy.add(t.ti);
					}
					creators++;
					String base = t.name.replaceFirst("^(Client|Server)", "");
					if (data.name.equals(t.name + "Data") || data.name.equals(base + "Data")) {
						named++;
					}
				}
			}
		}
		// Realm names: Client* classes should only have realm 0 creators, Server* realm 1.
		int[][] realmByPrefix = new int[2][2]; // [client/server prefix][realm]
		for (TypeRec t : types.values()) {
			int pre = t.name.startsWith("Client") ? 0 : t.name.startsWith("Server") ? 1 : -1;
			for (long[] c : t.creators) {
				if (pre >= 0 && c[1] < 2) {
					realmByPrefix[pre][(int) c[1]]++;
				}
			}
		}
		int agree = realmByPrefix[0][0] + realmByPrefix[1][1];
		int contra = realmByPrefix[0][1] + realmByPrefix[1][0];
		realmNamesValid = agree >= 20 && contra == 0;
		calibrationNotes.put("ClassInfo+0x50/+0x58", vtables + " data-class vtables confirmed, " + creators
			+ " creators found; the data class is named <Class>Data for " + named + " of them; "
			+ shortClassInfos + " ClassInfos are only 0x50 bytes (no slots)");
		calibrationNotes.put("creator realm", "Client* classes: realm0 " + realmByPrefix[0][0] + " / realm1 "
			+ realmByPrefix[0][1] + ", Server* classes: realm0 " + realmByPrefix[1][0] + " / realm1 "
			+ realmByPrefix[1][1] + " -> " + (realmNamesValid ? "realm 0 = client, 1 = server (USED)" : "kept as numbers"));
		return vtables;
	}

	private boolean realmNamesValid;

	private String realmName(long realm) {
		if (realmNamesValid && (realm == 0 || realm == 1)) {
			return realm == 0 ? "client" : "server";
		}
		return "realm" + realm;
	}

	/** Bytes from a to the next known record start (TypeInfo, TypeInfoData, field table). */
	private long recordRoom(long a) {
		Long n = knownRecordStarts().higher(a);
		return n == null ? Long.MAX_VALUE : n - a;
	}

	private static void confirmVftable(TypeRec t, long vt, long getTypeFn) {
		if (!t.vftableConfirmed) {
			t.vftables.clear();
			t.vftableConfirmed = true;
		}
		if (!t.vftables.contains(vt)) {
			t.vftables.add(vt);
		}
		t.getTypeFn = getTypeFn;
	}

	/**
	 * Only classes and value types (messages) have vtables, and never one with a reflected
	 * field at offset 0. Accessors of other types (Boolean, Int32, LinearTransform...) also
	 * sit in vtables, but those belong to non-reflected classes that reuse them (typed
	 * property tracks and the like), not to the type itself.
	 */
	private static boolean canHaveVtable(TypeRec t) {
		if (!t.category.equals("Class") && !t.category.equals("ValueType")) {
			return false;
		}
		for (Field fd : t.fields) {
			if (fd.offset == 0) {
				return false;
			}
		}
		return true;
	}

	/**
	 * "return &TypeInfo" functions by byte pattern, over all code:
	 *   48 8D 05 d32 C3              lea rax,[rip+d]; ret
	 *   48 8B 05 d32 C3              mov rax,[rip+d]; ret   (global holding the TypeInfo)
	 *   55 48 89 E5 48 8D 05 d32 5D C3   same with a frame pointer (PS4 clang)
	 *   55 48 89 E5 48 8B 05 d32 5D C3
	 * A match must look like a function start (16-aligned, after padding/ret, or an existing
	 * function), and outside executable blocks it must already be disassembled.
	 */
	private Map<Long, TypeRec> scanTypeAccessors() throws CancelledException {
		Map<Long, TypeRec> owner = new HashMap<>();
		final int chunk = 0x40000;
		final int overlap = 16;
		byte[] buf = new byte[chunk];
		for (MemoryBlock b : mem.getBlocks()) {
			if (!b.isInitialized() || !b.getStart().isMemoryAddress()) {
				continue;
			}
			long start = b.getStart().getOffset();
			long end = b.getEnd().getOffset() + 1;
			for (long a = start; a < end; a += chunk - overlap) {
				if (monitor.isCancelled()) {
					throw new CancelledException();
				}
				int n = (int) Math.min(chunk, end - a);
				int got;
				try {
					got = b.getBytes(toAddr(a), buf, 0, n);
				}
				catch (Exception e) {
					continue;
				}
				boolean last = a + n >= end;
				int limit = last ? got : Math.min(got, chunk - overlap);
				for (int i = 0; i < limit; i++) {
					int u = buf[i] & 0xff;
					if (u != 0x48 && u != 0x55) {
						continue;
					}
					long target = matchAccessor(buf, i, got, a + i);
					if (target == 0) {
						continue;
					}
					TypeRec t = types.get(target);
					long fn = a + i;
					if (t == null || owner.containsKey(fn)) {
						continue;
					}
					byte prev = i > 0 ? buf[i - 1] : (byte) 0xcc;
					boolean startLike = (fn & 0xf) == 0 || prev == (byte) 0xcc || prev == (byte) 0xc3
						|| prev == (byte) 0x90 || currentProgram.getFunctionManager().getFunctionAt(toAddr(fn)) != null;
					if (!startLike) {
						continue;
					}
					if (!b.isExecute() && currentProgram.getListing().getInstructionAt(toAddr(fn)) == null) {
						continue;
					}
					owner.put(fn, t);
					t.typeAccessors.add(fn);
				}
			}
		}
		return owner;
	}

	/** TypeInfo address returned by an accessor pattern at buf[i] (code address pc), or 0. */
	private long matchAccessor(byte[] buf, int i, int got, long pc) {
		int p = i;
		boolean frame = false;
		if (i + 12 <= got && (buf[i] & 0xff) == 0x55 && (buf[i + 1] & 0xff) == 0x48 && (buf[i + 2] & 0xff) == 0x89
			&& (buf[i + 3] & 0xff) == 0xe5) {
			p = i + 4;
			frame = true;
		}
		else if ((buf[i] & 0xff) != 0x48) {
			return 0;
		}
		int tail = frame ? 9 : 8; // bytes from p to the end of the pattern
		if (p + tail > got || (buf[p] & 0xff) != 0x48 || (buf[p + 2] & 0xff) != 0x05) {
			return 0;
		}
		int op = buf[p + 1] & 0xff;
		if (op != 0x8d && op != 0x8b) {
			return 0;
		}
		if (frame ? ((buf[p + 7] & 0xff) != 0x5d || (buf[p + 8] & 0xff) != 0xc3) : (buf[p + 7] & 0xff) != 0xc3) {
			return 0;
		}
		int disp = (buf[p + 3] & 0xff) | (buf[p + 4] & 0xff) << 8 | (buf[p + 5] & 0xff) << 16 | (buf[p + 6] << 24);
		long rip = pc + (p - i) + 7;
		long ea = rip + disp;
		return op == 0x8d ? ea : readQ(ea);
	}

	/**
	 * Data addresses referenced by the code of a function and (up to depth levels) the
	 * functions it calls, in execution-ish order. Walks the listing linearly from the entry
	 * until a RET, the next function, or an instruction cap.
	 */
	private List<Long> dataRefsFromCode(long entry, int depth) {
		Set<Long> out = new LinkedHashSet<>();
		dataRefsFromCode(entry, depth, out, new HashSet<>());
		return new ArrayList<>(out);
	}

	private void dataRefsFromCode(long entry, int depth, Set<Long> out, Set<Long> visited) {
		if (!visited.add(entry) || visited.size() > 32) {
			return;
		}
		Listing listing = currentProgram.getListing();
		FunctionManager fm = currentProgram.getFunctionManager();
		InstructionIterator it = listing.getInstructions(toAddr(entry), true);
		int n = 0;
		while (it.hasNext() && n++ < 256) {
			Instruction ins = it.next();
			long at = ins.getAddress().getOffset();
			if (at != entry && fm.getFunctionAt(ins.getAddress()) != null) {
				break;
			}
			for (Reference r : ins.getReferencesFrom()) {
				if (!r.getToAddress().isMemoryAddress()) {
					continue;
				}
				long to = r.getToAddress().getOffset();
				if (r.getReferenceType().isCall()) {
					if (depth > 0) {
						dataRefsFromCode(to, depth - 1, out, visited);
					}
				}
				else if (!r.getReferenceType().isFlow() && listing.getInstructionAt(r.getToAddress()) == null) {
					// Not filtered by block flags: a Scylla dump may mark the vtable section executable.
					out.add(to);
				}
			}
			if (ins.getMnemonicString().toUpperCase().startsWith("RET")) {
				break;
			}
		}
	}

	/** getType slot: index i at which create-stored vtables most often hold their own type's accessor. */
	private int voteGetTypeSlot(Map<TypeRec, List<Long>> createRefs, Map<Long, TypeRec> accessorOwner) {
		Map<Integer, Integer> votes = new TreeMap<>();
		for (Map.Entry<TypeRec, List<Long>> e : createRefs.entrySet()) {
			Set<Integer> mine = new HashSet<>();
			for (long vt : e.getValue()) {
				for (int i = 0; i < 64; i++) {
					long q = progQ(vt + 8L * i);
					if (q != 0 && accessorOwner.get(q) == e.getKey()) {
						mine.add(i);
					}
				}
			}
			for (int i : mine) {
				votes.merge(i, 1, Integer::sum);
			}
		}
		int best = -1;
		int bestCount = 0;
		for (Map.Entry<Integer, Integer> v : votes.entrySet()) {
			if (v.getValue() > bestCount) {
				best = v.getKey();
				bestCount = v.getValue();
			}
		}
		calibrationNotes.put("getType slot votes (slot -> create functions)", votes.toString());
		return bestCount >= 3 ? best : -1;
	}

	/**
	 * Data scan for pointers to accessors. Gives vftable candidates for classes whose vtable
	 * was not confirmed by a create function. If the getType slot is still unknown, returns
	 * the most common "walk back to the RTTI entry" index (works with Itanium RTTI or
	 * offset-to-top entries, not for back-to-back MSVC vtables without RTTI); else -1.
	 */
	private int findVtablesByScan(Map<Long, TypeRec> accessorOwner) throws CancelledException {
		if (accessorOwner.isEmpty()) {
			return -1;
		}
		// slot address -> accessor it points at. All initialized blocks: section flags in a
		// Scylla dump are unreliable (vtables may sit in a block marked executable).
		Map<Long, Long> slots = new TreeMap<>();
		byte[] buf = new byte[0x40000];
		for (MemoryBlock b : mem.getBlocks()) {
			if (!b.isInitialized() || !b.getStart().isMemoryAddress()) {
				continue;
			}
			long start = (b.getStart().getOffset() + 7) & ~7L;
			long end = b.getEnd().getOffset() + 1;
			for (long a = start; a + 8 <= end; a += buf.length) {
				if (monitor.isCancelled()) {
					throw new CancelledException();
				}
				int n = (int) Math.min(buf.length, end - a);
				int got;
				try {
					got = b.getBytes(toAddr(a), buf, 0, n);
				}
				catch (Exception e) {
					continue;
				}
				for (int i = 0; i + 8 <= got; i += 8) {
					long q = le64(buf, i);
					if (accessorOwner.containsKey(q)) {
						slots.put(a + i, q);
					}
				}
			}
		}

		int histogramBest = -1;
		Map<Long, Integer> slotIndex = new HashMap<>();
		if (getTypeVtableIndex < 0) {
			Map<Integer, Integer> hist = new TreeMap<>();
			for (long slot : slots.keySet()) {
				int idx = 0;
				while (idx < 512 && isCodePointer(progQ(slot - 8L * (idx + 1)))) {
					idx++;
				}
				slotIndex.put(slot, idx);
				hist.merge(idx, 1, Integer::sum);
			}
			int bestCount = 0;
			for (Map.Entry<Integer, Integer> e : hist.entrySet()) {
				if (e.getValue() > bestCount) {
					histogramBest = e.getKey();
					bestCount = e.getValue();
				}
			}
			calibrationNotes.put("getType slot histogram (walk back, slot -> count)", hist.toString());
		}
		int slot = getTypeVtableIndex >= 0 ? getTypeVtableIndex : histogramBest;
		if (slot < 0) {
			return -1;
		}
		for (Map.Entry<Long, Long> e : slots.entrySet()) {
			TypeRec t = accessorOwner.get(e.getValue());
			// Value types only get a vtable from their static instance: a scan hit for one is
			// as likely a non-reflected class reusing its accessor.
			if (t.vftableConfirmed || !canHaveVtable(t) || t.category.equals("ValueType")) {
				continue;
			}
			if (getTypeVtableIndex < 0 && slotIndex.get(e.getKey()) != slot) {
				continue; // secondary vtable or an unrelated function-pointer table
			}
			long vt = e.getKey() - 8L * slot;
			if (!t.vftables.contains(vt)) {
				t.vftables.add(vt);
			}
			t.getTypeFn = e.getValue();
		}
		return histogramBest;
	}

	/** True if v is code: an instruction or function starts there, or it is in an executable block. */
	private boolean isCodePointer(long v) {
		MemoryBlock b = blockAt(v);
		if (b == null || !b.isInitialized()) {
			return false;
		}
		if (b.isExecute()) {
			return true;
		}
		Address a = toAddr(v);
		return currentProgram.getListing().getInstructionAt(a) != null
			|| currentProgram.getFunctionManager().getFunctionAt(a) != null;
	}

	// ------------------------------------------------------------------
	// Output
	// ------------------------------------------------------------------

	/** Enum FieldInfoData keeps the value in the slot that holds the type pointer for fields (+0x10). */
	private static long enumValue(Field fd) {
		return (int) fd.typeTi;
	}

	private String typeName(long ti) {
		return typeName(ti, 0);
	}

	/** Unique identifier of a type, or "" if the address is not a known type. */
	private String identOf(long ti) {
		TypeRec t = types.get(ti);
		return t == null ? "" : t.ident;
	}

	private String typeName(long ti, int depth) {
		TypeRec t = types.get(ti);
		if (t == null) {
			return ti == 0 ? "void" : "?" + hex(ti);
		}
		if (depth > 8) {
			return t.name; // cyclic element chain from emulation garbage
		}
		if (t.category.equals("Array") && t.elementTi != 0 && t.elementTi != ti) {
			return "Array<" + typeName(t.elementTi, depth + 1) + ">";
		}
		if (t.category.equals("FixedArray") && t.name.startsWith("Unnamed_") && t.elementTi != 0
			&& t.elementTi != ti) {
			return "FixedArray<" + typeName(t.elementTi, depth + 1) + ">";
		}
		return t.name;
	}

	private List<TypeRec> sortedTypes() {
		List<TypeRec> list = new ArrayList<>(types.values());
		list.sort(Comparator.comparing((TypeRec t) -> t.category).thenComparing(t -> t.name)
			.thenComparingLong(t -> t.ti));
		return list;
	}

	private void writeText(File f) throws IOException {
		try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
			w.println("# Frostbite TypeInfo dump - " + currentProgram.getName());
			w.println("# " + types.size() + " types. Offsets in hex. ti = runtime TypeInfo, data = static TypeInfoData.");
			w.println();
			for (TypeRec t : sortedTypes()) {
				w.print(t.category + " " + typeName(t.ti));
				if (t.superTi != 0) {
					w.print(" : " + typeName(t.superTi));
				}
				if (t.elementTi != 0) {
					w.print(" of " + typeName(t.elementTi));
				}
				w.print("   // size 0x" + Integer.toHexString(t.size) + ", align " + t.alignment + ", flags 0x"
					+ Integer.toHexString(t.flags) + ", ti " + hex(t.ti) + ", data " + hex(t.data));
				if (!t.module.isEmpty()) {
					w.print(", module " + t.module);
				}
				if (runtimeIdsValid && t.runtimeId >= 0) {
					w.print(", id " + t.runtimeId);
				}
				if (classTreeValid && t.classId >= 0) {
					w.print(", classId " + t.classId + ".." + t.lastSubClassId);
				}
				for (long[] c : t.creators) {
					w.print(", created from " + typeName(c[2]) + " (" + realmName(c[1]) + ", creator " + hex(c[0]) + ")");
				}
				if (!t.createdBy.isEmpty()) {
					List<String> n = new ArrayList<>();
					for (long r : t.createdBy) {
						n.add(typeName(r));
					}
					w.print(", creates " + String.join("/", n));
				}
				if (t.getTypeFn != 0) {
					w.print(", getType " + hex(t.getTypeFn));
				}
				for (long v : t.vftables) {
					w.print(", vftable " + hex(v));
				}
				if (t.createFn != 0) {
					w.print(", create " + hex(t.createFn));
				}
				w.println(t.note.isEmpty() ? "" : ", " + t.note);
				List<Field> fs = new ArrayList<>(t.fields);
				if (!t.category.equals("Enum")) {
					fs.sort(Comparator.comparingInt(x -> x.offset));
				}
				for (Field fd : fs) {
					if (t.category.equals("Enum")) {
						w.println("    " + fd.name + " = " + enumValue(fd));
					}
					else {
						w.println("    +0x" + pad(Integer.toHexString(fd.offset), 4) + " " + fd.name + " : "
							+ typeName(fd.typeTi) + "   // flags 0x" + Integer.toHexString(fd.flags)
							+ (fd.raw0C != 0 ? ", +0C 0x" + Long.toHexString(fd.raw0C) : ""));
					}
				}
				w.println();
			}
			w.println("# Initializer emulation log (" + rootSource + ")");
			for (String s : rootLog) {
				w.println("# " + s);
			}
		}
	}

	private void writeJson(File f) throws IOException {
		try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
			w.println("{");
			w.println("  \"program\": " + js(currentProgram.getName()) + ",");
			w.println("  \"typeCount\": " + types.size() + ",");
			w.println("  \"getTypeVtableIndex\": " + getTypeVtableIndex + ",");
			w.println("  \"types\": [");
			List<TypeRec> list = sortedTypes();
			for (int i = 0; i < list.size(); i++) {
				TypeRec t = list.get(i);
				StringBuilder sb = new StringBuilder();
				sb.append("    {\"name\": ").append(js(typeName(t.ti)));
				sb.append(", \"ident\": ").append(js(t.ident));
				sb.append(", \"category\": ").append(js(t.category));
				sb.append(", \"kind\": ").append(kind(t.flags));
				sb.append(", \"size\": ").append(t.size);
				sb.append(", \"alignment\": ").append(t.alignment);
				sb.append(", \"flags\": ").append(t.flags);
				sb.append(", \"ti\": ").append(js(hex(t.ti)));
				sb.append(", \"data\": ").append(js(hex(t.data)));
				if (!t.module.isEmpty()) {
					sb.append(", \"module\": ").append(js(t.module));
				}
				if (runtimeIdsValid && t.runtimeId >= 0) {
					sb.append(", \"runtimeId\": ").append(t.runtimeId);
				}
				if (classTreeValid && t.classId >= 0) {
					// isKindOf(X) on this build: X.classId <= obj.classId <= X.lastSubClassId
					sb.append(", \"classId\": ").append(t.classId);
					sb.append(", \"lastSubClassId\": ").append(t.lastSubClassId);
				}
				if (t.defaultInstance != 0) {
					sb.append(", \"defaultInstance\": ").append(js(hex(t.defaultInstance)));
				}
				if (t.staticInstance != 0) {
					sb.append(", \"staticInstance\": ").append(js(hex(t.staticInstance)));
				}
				else if (t.valueTypeUnk20 != 0) {
					sb.append(", \"unk20\": ").append(js(hex(t.valueTypeUnk20)));
				}
				if (!t.creators.isEmpty()) {
					sb.append(", \"createdFrom\": [");
					for (int j = 0; j < t.creators.size(); j++) {
						long[] c = t.creators.get(j);
						sb.append(j > 0 ? ", " : "").append("{\"data\": ").append(js(typeName(c[2])))
							.append(", \"dataIdent\": ").append(js(identOf(c[2])))
							.append(", \"realm\": ").append(c[1])
							.append(", \"side\": ").append(js(realmName(c[1])))
							.append(", \"creator\": ").append(js(hex(c[0]))).append("}");
					}
					sb.append("]");
				}
				if (!t.createdBy.isEmpty()) {
					sb.append(", \"creates\": [");
					for (int j = 0; j < t.createdBy.size(); j++) {
						sb.append(j > 0 ? ", " : "").append(js(typeName(t.createdBy.get(j))));
					}
					sb.append("]");
				}
				if (!t.note.isEmpty()) {
					sb.append(", \"note\": ").append(js(t.note));
				}
				// Names are not unique (same name in two modules), so references also carry
				// the TypeInfo address and the unique ident.
				if (t.superTi != 0) {
					sb.append(", \"super\": ").append(js(typeName(t.superTi)));
					sb.append(", \"superTi\": ").append(js(hex(t.superTi)));
					sb.append(", \"superIdent\": ").append(js(identOf(t.superTi)));
				}
				if (t.elementTi != 0) {
					sb.append(", \"element\": ").append(js(typeName(t.elementTi)));
					sb.append(", \"elementTi\": ").append(js(hex(t.elementTi)));
					sb.append(", \"elementIdent\": ").append(js(identOf(t.elementTi)));
				}
				if (t.getTypeFn != 0) {
					sb.append(", \"getType\": ").append(js(hex(t.getTypeFn)));
				}
				if (!t.vftables.isEmpty()) {
					sb.append(", \"vftables\": [");
					for (int j = 0; j < t.vftables.size(); j++) {
						sb.append(j > 0 ? ", " : "").append(js(hex(t.vftables.get(j))));
					}
					sb.append("]");
				}
				if (t.createFn != 0) {
					sb.append(", \"create\": ").append(js(hex(t.createFn)));
				}
				sb.append(", \"fields\": [");
				for (int j = 0; j < t.fields.size(); j++) {
					Field fd = t.fields.get(j);
					if (j > 0) {
						sb.append(", ");
					}
					sb.append("{\"name\": ").append(js(fd.name));
					sb.append(", \"offset\": ").append(fd.offset);
					sb.append(", \"flags\": ").append(fd.flags);
					if (t.category.equals("Enum")) {
						sb.append(", \"value\": ").append(enumValue(fd));
					}
					else {
						sb.append(", \"type\": ").append(js(typeName(fd.typeTi)));
						sb.append(", \"typeTi\": ").append(js(hex(fd.typeTi)));
						sb.append(", \"typeIdent\": ").append(js(identOf(fd.typeTi)));
					}
					if (fd.raw0C != 0) {
						sb.append(", \"unk0C\": ").append(fd.raw0C);
					}
					sb.append("}");
				}
				sb.append("]}");
				if (i + 1 < list.size()) {
					sb.append(",");
				}
				w.println(sb);
			}
			w.println("  ],");
			w.println("  \"calibration\": {");
			int k = 0;
			for (Map.Entry<String, String> e : calibrationNotes.entrySet()) {
				w.println("    " + js(e.getKey()) + ": " + js(e.getValue()) + (++k < calibrationNotes.size() ? "," : ""));
			}
			w.println("  }");
			w.println("}");
		}
	}

	/**
	 * name -> {C type, size}. Used only when the primitive's own record has the same size
	 * (Catalyst's String is 0x20 bytes, not a pointer); otherwise the primitive becomes an
	 * opaque blob of its recorded size.
	 */
	static final Map<String, String[]> PRIMITIVE_C = new HashMap<>();
	static {
		PRIMITIVE_C.put("Boolean", new String[] { "bool", "1" });
		PRIMITIVE_C.put("Int8", new String[] { "int8_t", "1" });
		PRIMITIVE_C.put("Uint8", new String[] { "uint8_t", "1" });
		PRIMITIVE_C.put("Int16", new String[] { "int16_t", "2" });
		PRIMITIVE_C.put("Uint16", new String[] { "uint16_t", "2" });
		PRIMITIVE_C.put("Int32", new String[] { "int32_t", "4" });
		PRIMITIVE_C.put("Uint32", new String[] { "uint32_t", "4" });
		PRIMITIVE_C.put("Int64", new String[] { "int64_t", "8" });
		PRIMITIVE_C.put("Uint64", new String[] { "uint64_t", "8" });
		PRIMITIVE_C.put("Float32", new String[] { "float", "4" });
		PRIMITIVE_C.put("Float64", new String[] { "double", "8" });
		PRIMITIVE_C.put("CString", new String[] { "const char*", "8" });
		PRIMITIVE_C.put("String", new String[] { "void* /*fb::String*/", "8" });
		PRIMITIVE_C.put("FileRef", new String[] { "const char* /*FileRef*/", "8" });
		PRIMITIVE_C.put("ResourceRef", new String[] { "uint64_t /*ResourceRef*/", "8" });
		PRIMITIVE_C.put("TypeRef", new String[] { "void* /*TypeRef*/", "8" });
	}

	/**
	 * Type code -> primitive name (FB3 TypeEnum). Every code was confirmed against the PC
	 * dump's TypeInfoData flags. Names without a PRIMITIVE_C entry (Void, DbObject, Guid,
	 * SHA1, BoxedValueRef) become blobs of the record's size; Guid gets its real layout.
	 */
	static final Map<Integer, String> KIND_PRIMITIVE = new HashMap<>();
	static {
		KIND_PRIMITIVE.put(0, "Void");
		KIND_PRIMITIVE.put(1, "DbObject");
		KIND_PRIMITIVE.put(6, "String");
		KIND_PRIMITIVE.put(7, "CString");
		KIND_PRIMITIVE.put(9, "FileRef");
		KIND_PRIMITIVE.put(10, "Boolean");
		KIND_PRIMITIVE.put(11, "Int8");
		KIND_PRIMITIVE.put(12, "Uint8");
		KIND_PRIMITIVE.put(13, "Int16");
		KIND_PRIMITIVE.put(14, "Uint16");
		KIND_PRIMITIVE.put(15, "Int32");
		KIND_PRIMITIVE.put(16, "Uint32");
		KIND_PRIMITIVE.put(17, "Int64");
		KIND_PRIMITIVE.put(18, "Uint64");
		KIND_PRIMITIVE.put(19, "Float32");
		KIND_PRIMITIVE.put(20, "Float64");
		KIND_PRIMITIVE.put(21, "Guid");
		KIND_PRIMITIVE.put(22, "SHA1");
		KIND_PRIMITIVE.put(23, "ResourceRef");
		KIND_PRIMITIVE.put(25, "TypeRef");
	}

	/** Identifiers that would not compile (C++ keywords) or clash with the header's own names. */
	static final Set<String> RESERVED_IDENTS = new HashSet<>(Arrays.asList(
		"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
		"catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval",
		"constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
		"default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
		"false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
		"noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
		"register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
		"static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
		"try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
		"wchar_t", "while", "xor", "xor_eq",
		"Array", "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
		"size_t", "__vftable"));

	/** Primitive name: its own, or the one its type code stands for. */
	private static String primitiveName(TypeRec t) {
		if (PRIMITIVE_C.containsKey(t.name)) {
			return t.name;
		}
		String byKind = KIND_PRIMITIVE.get(kind(t.flags));
		return byKind != null ? byKind : t.name;
	}

	/** C mapping for a primitive, or null if there is none or its size disagrees with the record. */
	private static String[] primitiveC(TypeRec t) {
		String[] c = PRIMITIVE_C.get(primitiveName(t));
		if (c == null || (t.size > 0 && t.size != Integer.parseInt(c[1]))) {
			return null;
		}
		return c;
	}

	/** A primitive with a recorded size but no usable C mapping: emitted as a struct of its own. */
	private static boolean isBlobPrimitive(TypeRec t) {
		return t.category.equals("Primitive") && t.size > 0 && primitiveC(t) == null;
	}

	private static boolean isStructType(TypeRec t) {
		return t.category.equals("Class") || t.category.equals("ValueType");
	}

	private static int enumSize(TypeRec t) {
		return (t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8) ? t.size : 4;
	}

	private static String enumUnderlying(TypeRec t) {
		int sz = enumSize(t);
		if (sz == 4) {
			return "int32_t";
		}
		boolean negative = false;
		for (Field fd : t.fields) {
			negative |= enumValue(fd) < 0;
		}
		return (negative ? "int" : "uint") + (sz * 8) + "_t";
	}

	/** C type and byte size for a value of the given TypeInfo, or null if unknown. */
	private String[] cTypeOf(long ti, int depth) {
		TypeRec t = types.get(ti);
		if (t == null || depth > 8) {
			return null;
		}
		switch (t.category) {
			case "Class":
				return new String[] { t.ident + "*", "8" };
			case "Array":
				return new String[] { "fb::Array<" + (t.elementTi != 0 ? cElem(t.elementTi, depth + 1) : "void") + ">", "8" };
			case "Enum":
				return new String[] { t.ident, Integer.toString(enumSize(t)) };
			case "ValueType":
				if (t.size > 0) {
					return new String[] { t.ident, Integer.toString(t.size) };
				}
				return null;
			case "Primitive":
				String[] prim = primitiveC(t);
				if (prim != null) {
					return prim;
				}
				return t.size > 0 ? new String[] { t.ident, Integer.toString(t.size) } : null;
			default:
				// FixedArray (inline storage), Function, Delegate...: a byte blob of the record's size.
				return null;
		}
	}

	private String cElem(long ti, int depth) {
		String[] c = cTypeOf(ti, depth);
		return c == null ? "void" : c[0].replaceAll("/\\*.*?\\*/", "").trim();
	}

	/** C type for a field: from the field's TypeInfo; the array flag only helps when that is missing. */
	private String[] fieldCType(Field fd, Set<Long> emitted) {
		TypeRec ft = types.get(fd.typeTi);
		if (ft == null) {
			return fd.isArrayFlag() ? new String[] { "fb::Array<void>", "8" } : null;
		}
		if (ft.category.equals("Array")) {
			return arrayCType(fd);
		}
		if (ft.category.equals("ValueType") && emitted != null && !emitted.contains(ft.ti)) {
			return null; // incomplete here (cycle): fall back to a byte blob
		}
		return cTypeOf(fd.typeTi, 0);
	}

	private static String alignSpec(TypeRec t) {
		int a = t.alignment;
		return (a > 8 && a <= 64 && Integer.bitCount(a) == 1) ? "alignas(" + a + ") " : "";
	}

	/** True for a class with no reflected base: its object starts with the vtable pointer. */
	private boolean isRootClass(TypeRec t) {
		if (!t.category.equals("Class")) {
			return false;
		}
		TypeRec sup = t.superTi != 0 ? types.get(t.superTi) : null;
		return sup == null || !isStructType(sup) || sup.ti == t.ti;
	}

	/**
	 * Objects that start with their own vtable pointer: root classes, and value types with a
	 * confirmed vtable (messages).
	 */
	private boolean hasOwnVftablePointer(TypeRec t) {
		boolean message = t.category.equals("ValueType") && t.vftableConfirmed && t.vftables.size() == 1;
		return (isRootClass(t) || message) && hasRoomForVftable(t);
	}

	/** Lowest field offset (or the size if there are no fields): room for a vtable pointer? */
	private static boolean hasRoomForVftable(TypeRec t) {
		int first = t.size;
		for (Field fd : t.fields) {
			first = Math.min(first, fd.offset);
		}
		return first >= 8;
	}

	private void writeHeader(File f) throws IOException {
		try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
			w.println("// Generated by FrostbiteTypeDumper from " + currentProgram.getName());
			w.println("// Offsets come from the engine's own reflection data. Sizes of types whose");
			w.println("// TypeInfoData size is unknown are padded conservatively; check static_asserts.");
			w.println("#pragma once");
			w.println("#include <cstdint>");
			w.println("#include <cstddef>");
			w.println();
			w.println("namespace fb {");
			w.println("template <typename T> struct Array { T* data; }; // Frostbite Array<T>: pointer to [count:u32 @ -4][elements]");
			w.println();

			// Type identifiers, so fields that share a type's name can be renamed (C++ forbids
			// `Array<Foo*> Foo;` inside a class: "declaration changes meaning").
			typeIdents.clear();
			for (TypeRec t : types.values()) {
				typeIdents.add(t.ident);
			}

			// Primitives whose recorded size has no plain C equivalent (String 0x20, DbObject,
			// Guid, SHA1...): opaque structs of the right size.
			w.println("// ---- primitives without a plain C type ----");
			for (TypeRec t : sortedTypes()) {
				if (!isBlobPrimitive(t)) {
					continue;
				}
				if (primitiveName(t).equals("Guid") && t.size == 16) {
					w.println("struct " + t.ident + " { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };");
				}
				else {
					w.println("struct " + t.ident + " { uint8_t _[0x" + Integer.toHexString(t.size) + "]; }; // "
						+ t.name);
				}
				w.println("static_assert(sizeof(" + t.ident + ") == 0x" + Integer.toHexString(t.size) + ", \"size\");");
			}
			w.println();

			// Enums: they depend on nothing, so struct fields can use them directly.
			w.println("// ---- enums ----");
			for (TypeRec t : sortedTypes()) {
				if (!t.category.equals("Enum")) {
					continue;
				}
				w.println("enum class " + t.ident + " : " + enumUnderlying(t) + " {");
				Set<String> seen = new HashSet<>();
				for (Field fd : t.fields) {
					w.println("    " + uniqueIdent(cIdent(fd.name), seen, false) + " = " + enumValue(fd) + ",");
				}
				w.println("};");
			}
			w.println();

			// Forward declarations.
			List<TypeRec> emitList = new ArrayList<>();
			for (TypeRec t : sortedTypes()) {
				if (isStructType(t)) {
					emitList.add(t);
					w.println("struct " + t.ident + ";");
				}
			}
			w.println();

			// Emit in dependency order (super classes and embedded value types first).
			Set<Long> emitted = new HashSet<>();
			Set<Long> inProgress = new HashSet<>();
			for (TypeRec t : emitList) {
				emitStruct(w, t, emitted, inProgress);
			}
			w.println("} // namespace fb");
		}
	}

	private void emitStruct(PrintWriter w, TypeRec t, Set<Long> emitted, Set<Long> inProgress) {
		if (emitted.contains(t.ti) || inProgress.contains(t.ti)) {
			return;
		}
		inProgress.add(t.ti);
		TypeRec sup = t.superTi != 0 ? types.get(t.superTi) : null;
		if (sup != null && isStructType(sup)) {
			emitStruct(w, sup, emitted, inProgress);
		}
		// Value types are embedded by value, so their definitions must come first.
		for (Field fd : t.fields) {
			TypeRec ft = types.get(fd.typeTi);
			if (ft != null && ft.category.equals("ValueType")) {
				emitStruct(w, ft, emitted, inProgress);
			}
		}
		inProgress.remove(t.ti);

		int start = 0;
		String header = "struct " + alignSpec(t) + t.ident;
		if (sup != null && isStructType(sup) && emitted.contains(sup.ti) && (t.size == 0 || sup.size <= t.size)) {
			header += " : " + sup.ident;
			start = sup.size;
		}
		w.println("// " + t.category + " " + t.name + "  size 0x" + Integer.toHexString(t.size) + "  ti " + hex(t.ti)
			+ (t.module.isEmpty() ? "" : "  module " + t.module)
			+ (classTreeValid && t.classId >= 0 ? "  classId " + t.classId + ".." + t.lastSubClassId : ""));
		for (long[] c : t.creators) {
			w.println("// created from " + typeName(c[2]) + " (" + realmName(c[1]) + ")");
		}
		w.println(header + " {");

		List<Field> fs = new ArrayList<>(t.fields);
		fs.sort(Comparator.comparingInt(x -> x.offset));
		Set<String> seen = new HashSet<>();
		int cursor = start;
		int padIdx = 0;
		if (start == 0 && hasOwnVftablePointer(t)) {
			w.println("    void* __vftable; // +0x0" + (t.vftables.size() == 1 ? " " + hex(t.vftables.get(0)) : ""));
			seen.add("__vftable");
			cursor = 8;
		}
		for (Field fd : fs) {
			TypeRec ft = types.get(fd.typeTi);
			String[] c = fieldCType(fd, emitted);
			if (fd.offset < cursor) {
				w.println("    // overlapping/bitfield: +0x" + Integer.toHexString(fd.offset) + " " + fd.name + " : "
					+ typeName(fd.typeTi));
				continue;
			}
			if (fd.offset > cursor) {
				w.println("    uint8_t " + uniqueIdent("_pad" + (padIdx++), seen, false) + "[0x"
					+ Integer.toHexString(fd.offset - cursor) + "];");
				cursor = fd.offset;
			}
			if (c == null) {
				if (ft != null && ft.size > 0) {
					w.println("    uint8_t " + memberIdent(fd.name, seen) + "[0x" + Integer.toHexString(ft.size) + "]; // +0x"
						+ Integer.toHexString(fd.offset) + " " + typeName(fd.typeTi));
					cursor += ft.size;
					continue;
				}
				w.println("    // +0x" + Integer.toHexString(fd.offset) + " " + fd.name + " : " + typeName(fd.typeTi)
					+ " (unknown size)");
				continue;
			}
			w.println("    " + c[0] + " " + memberIdent(fd.name, seen) + "; // +0x" + Integer.toHexString(fd.offset)
				+ ((fd.flags & FIELD_FLAG_SETTING) != 0 ? " (setting)" : ""));
			cursor += Integer.parseInt(c[1]);
		}
		if (t.size > cursor) {
			w.println("    uint8_t " + uniqueIdent("_pad" + (padIdx++), seen, false) + "[0x"
				+ Integer.toHexString(t.size - cursor) + "];");
		}
		w.println("};");
		if (t.size > 0 && cursor <= t.size) {
			w.println("static_assert(sizeof(" + t.ident + ") == 0x" + Integer.toHexString(t.size) + ", \"size\");");
		}
		w.println();
		emitted.add(t.ti);
	}

	private final Set<String> typeIdents = new HashSet<>();

	/** C++ member name: unique within its struct, not a keyword, not the name of a type. */
	private String memberIdent(String fieldName, Set<String> seen) {
		return uniqueIdent(cIdent(fieldName), seen, true);
	}

	private String uniqueIdent(String id, Set<String> seen, boolean avoidTypes) {
		if (RESERVED_IDENTS.contains(id) || (avoidTypes && typeIdents.contains(id))) {
			id += "_";
		}
		String u = id;
		int n = 2;
		while (seen.contains(u)) {
			u = id + "_" + (n++);
		}
		seen.add(u);
		return u;
	}

	private String[] arrayCType(Field fd) {
		TypeRec at = types.get(fd.typeTi);
		String elem = "void";
		if (at != null && at.elementTi != 0) {
			elem = cElem(at.elementTi, 0);
		}
		return new String[] { "fb::Array<" + elem + ">", "8" };
	}

	private void writeCalibration(File f) throws IOException {
		try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
			w.println("# Layout evidence. Send this file back if offsets look wrong.");
			w.println();
			w.println("Initializer roots: " + rootSource);
			w.println();
			w.println("Calibration notes:");
			for (Map.Entry<String, String> e : calibrationNotes.entrySet()) {
				w.println("  " + e.getKey() + ": " + e.getValue());
			}
			w.println();
			w.println("Annotation (empty if the program was not annotated):");
			for (Map.Entry<String, Integer> e : annotateStats.entrySet()) {
				w.println("  " + e.getKey() + " : " + e.getValue());
			}
			w.println();
			Map<Long, Integer> helperHist = new TreeMap<>();
			for (TypeRec t : types.values()) {
				helperHist.merge(t.helper, 1, Integer::sum);
			}
			w.println("Registration helpers seen (entry -> count, category):");
			for (Map.Entry<Long, Integer> e : helperHist.entrySet()) {
				w.println("  " + hex(e.getKey()) + " -> " + e.getValue() + "  " + helperCategory.getOrDefault(e.getKey(), "?"));
			}
			w.println();
			Map<String, Integer> fieldOffHist = new TreeMap<>();
			for (TypeRec t : types.values()) {
				fieldOffHist.merge(t.category + " fieldsPtr@+0x" + Integer.toHexString(Math.max(t.fieldsPtrOff, 0))
					+ (t.fieldsPtrOff < 0 ? " (none)" : ""), 1, Integer::sum);
			}
			w.println("Field table pointer slot per category:");
			for (Map.Entry<String, Integer> e : fieldOffHist.entrySet()) {
				w.println("  " + e.getKey() + " : " + e.getValue());
			}
			w.println();
			Map<String, Integer> flagHist = new TreeMap<>();
			int nonZero0C = 0;
			int fieldTotal = 0;
			for (TypeRec t : types.values()) {
				if (t.category.equals("Enum")) {
					continue;
				}
				for (Field fd : t.fields) {
					TypeRec ft = types.get(fd.typeTi);
					flagHist.merge("0x" + pad(Integer.toHexString(fd.flags), 4) + " " + (ft == null ? "?" : ft.category),
						1, Integer::sum);
					nonZero0C += fd.raw0C != 0 ? 1 : 0;
					fieldTotal++;
				}
			}
			w.println("Field flags x field type category -> count (expected: 0x0000 any, 0x0048 Array, 0x2000 settings):");
			for (Map.Entry<String, Integer> e : flagHist.entrySet()) {
				w.println("  " + e.getKey() + " -> " + e.getValue());
			}
			w.println("FieldInfoData +0x0C non-zero in " + nonZero0C + " of " + fieldTotal + " fields"
				+ (nonZero0C == 0 ? " (padding)" : " (see fb_types.txt, '+0C')"));
			w.println();
			Map<String, Integer> modHist = new TreeMap<>();
			int noModule = 0;
			for (TypeRec t : types.values()) {
				if (t.module.isEmpty()) {
					noModule++;
				}
				else {
					modHist.merge(t.module, 1, Integer::sum);
				}
			}
			w.println("Modules (TypeInfoData+0x10 -> ModuleInfo, name assumed at +0; unconfirmed): " + modHist.size()
				+ " names, " + noModule + " types without one");
			for (Map.Entry<String, Integer> e : modHist.entrySet()) {
				w.println("  " + e.getKey() + " -> " + e.getValue());
			}
			w.println();
			w.println("Raw enum FieldInfoData entries (to confirm where the value lives):");
			int shownEnums = 0;
			for (TypeRec t : sortedTypes()) {
				if (!t.category.equals("Enum") || t.fieldsPtr == 0 || shownEnums >= 4) {
					continue;
				}
				shownEnums++;
				w.println("  " + t.name + " (" + t.fields.size() + " values) table " + hex(t.fieldsPtr) + ":");
				for (int i = 0; i < Math.min(4, t.fields.size()); i++) {
					long e = t.fieldsPtr + (long) i * FIELD_STRIDE;
					w.println("    [" + i + "] " + t.fields.get(i).name + " : +08 " + pad(Long.toHexString(readQ(e + 8)), 16)
						+ "  +10 " + pad(Long.toHexString(readQ(e + 0x10)), 16));
				}
			}
			w.println();
			w.println("Raw TypeInfoData (first 0x50 bytes) for up to 4 types per category:");
			Map<String, Integer> shown = new HashMap<>();
			for (TypeRec t : sortedTypes()) {
				int s = shown.getOrDefault(t.category, 0);
				if (s >= 4 || t.data == 0) {
					continue;
				}
				shown.put(t.category, s + 1);
				w.println("  " + t.category + " " + t.name + " data " + hex(t.data) + ":");
				for (int off = 0; off < 0x50; off += 8) {
					long q = readQ(t.data + off);
					String note = "";
					if (types.containsKey(q)) {
						note = "  -> TI " + types.get(q).name;
					}
					else {
						String str = readCString(q);
						if (str != null) {
							note = "  -> \"" + str + "\"";
						}
					}
					w.println("    +0x" + pad(Integer.toHexString(off), 2) + ": " + pad(Long.toHexString(q), 16) + note);
				}
			}
			w.println();
			// Runtime ids (TypeInfo+0x10) and the ClassInfo fields after super (classId /
			// lastSubClassId, used by isKindOf range checks) are not decoded yet: raw evidence.
			w.println("Raw runtime ClassInfo (0x60 bytes, up to 6) and TypeInfo (0x20 bytes, 2 per other category):");
			Map<String, Integer> shownTi = new HashMap<>();
			for (TypeRec t : sortedTypes()) {
				int s = shownTi.getOrDefault(t.category, 0);
				if (s >= (t.category.equals("Class") ? 6 : 2) || !isReadable(t.ti)) {
					continue;
				}
				shownTi.put(t.category, s + 1);
				w.println("  " + t.category + " " + t.name + " ti " + hex(t.ti) + ":");
				int len = t.category.equals("Class") ? 0x60 : 0x20;
				for (int off = 0; off < len; off += 8) {
					long q = readQ(t.ti + off);
					String note = types.containsKey(q) ? "  -> TI " + types.get(q).name : "";
					w.println("    +0x" + pad(Integer.toHexString(off), 2) + ": " + pad(Long.toHexString(q), 16)
						+ "  (u16 " + (q & 0xffff) + ", " + ((q >>> 16) & 0xffff) + ", " + ((q >>> 32) & 0xffff) + ", "
						+ ((q >>> 48) & 0xffff) + ")" + note);
				}
			}
		}
	}

	// ------------------------------------------------------------------
	// Ghidra data types
	// ------------------------------------------------------------------

	private DataType dtTypeInfo;
	private DataType dtClassInfo;
	private DataType dtTypeInfoData;
	private DataType dtClassInfoData;
	private DataType dtValueTypeInfoData;
	private DataType dtEnumInfoData;
	private DataType dtArrayInfoData;
	private DataType dtFieldInfoData;
	private DataType dtCreator;
	private DataType dtTypeInfoBase;
	private DataType dtClassInfoEx;
	/** ti -> resolved /fb struct or enum. */
	private final Map<Long, DataType> fbTypes = new HashMap<>();
	/** ti -> resolved /fb/vtables/X_vtbl (classes with exactly one vftable). */
	private final Map<Long, DataType> vtblTypes = new HashMap<>();
	/** vftable address -> number of slots. */
	private final Map<Long, Integer> vtableLength = new HashMap<>();

	/**
	 * Builds /fb (one struct per Class/ValueType with the engine's exact offsets, one enum per
	 * Enum, opaque structs for primitives without a plain C type), /fb/vtables (one struct of
	 * function pointers per vftable) and /fb/reflection (the reflection records themselves).
	 * Existing types of the same name are replaced, so re-runs update instead of duplicating.
	 */
	private void createDataTypes() {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		CategoryPath fbCat = new CategoryPath(FB_CATEGORY);
		createReflectionTypes(dtm, new CategoryPath(REFLECTION_CATEGORY));
		createVtableTypes(dtm, new CategoryPath(VTABLE_CATEGORY));

		// Phase 1: enums, blob primitives, and struct placeholders of the engine's size so
		// value types can be embedded before they are filled in.
		for (TypeRec t : sortedTypes()) {
			if (t.category.equals("Enum")) {
				EnumDataType e = new EnumDataType(fbCat, t.ident, enumSize(t), dtm);
				Set<String> seen = new HashSet<>();
				for (Field fd : t.fields) {
					try {
						e.add(uniqueIdent(cIdent(fd.name), seen, false), enumValue(fd));
					}
					catch (Exception ex) {
						count("enum values rejected by Ghidra");
					}
				}
				fbTypes.put(t.ti, dtm.addDataType(e, DataTypeConflictHandler.REPLACE_HANDLER));
			}
			else if (isBlobPrimitive(t)) {
				StructureDataType s = new StructureDataType(fbCat, t.ident, t.size, dtm);
				if (primitiveName(t).equals("Guid") && t.size == 16) {
					s.replaceAtOffset(0, UnsignedIntegerDataType.dataType, 4, "d1", null);
					s.replaceAtOffset(4, UnsignedShortDataType.dataType, 2, "d2", null);
					s.replaceAtOffset(6, UnsignedShortDataType.dataType, 2, "d3", null);
					s.replaceAtOffset(8, new ArrayDataType(ByteDataType.dataType, 8, 1, dtm), 8, "d4", null);
				}
				fbTypes.put(t.ti, dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER));
			}
			else if (isStructType(t)) {
				StructureDataType s = new StructureDataType(fbCat, t.ident, Math.max(t.size, 0), dtm);
				fbTypes.put(t.ti, dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER));
			}
		}

		// Phase 2: fill the structs.
		for (TypeRec t : sortedTypes()) {
			DataType dt = fbTypes.get(t.ti);
			if (dt instanceof Structure && isStructType(t)) {
				fillStructure((Structure) dt, t, dtm);
			}
		}
		println("FrostbiteTypeDumper: created " + fbTypes.size() + " data types in " + FB_CATEGORY + " and "
			+ vtblTypes.size() + " vtable structs in " + VTABLE_CATEGORY);
	}

	/**
	 * X_vtbl for every class with exactly one vftable. Without RTTI, vtables sit back to back,
	 * so a vtable ends at the first slot that is not code, is the start of another known
	 * record or vftable, or is referenced from anywhere (vtable starts are referenced by the
	 * constructors and destructors; slots inside a vtable are not).
	 */
	private void createVtableTypes(DataTypeManager dtm, CategoryPath cat) {
		TreeSet<Long> stops = new TreeSet<>(knownRecordStarts());
		for (TypeRec t : types.values()) {
			stops.addAll(t.vftables);
		}
		ReferenceManager rm = currentProgram.getReferenceManager();
		for (TypeRec t : sortedTypes()) {
			for (long vt : t.vftables) {
				int n = 0;
				while (n < 512) {
					long slot = vt + 8L * n;
					if (n > 0 && (stops.contains(slot) || rm.hasReferencesTo(toAddr(slot)))) {
						break;
					}
					if (!isCodePointer(progQ(slot))) {
						break;
					}
					n++;
				}
				if (n > 0) {
					vtableLength.put(vt, n);
				}
			}
			if (t.vftables.size() != 1 || !vtableLength.containsKey(t.vftables.get(0))) {
				continue;
			}
			int n = vtableLength.get(t.vftables.get(0));
			StructureDataType s = new StructureDataType(cat, t.ident + "_vtbl", n * 8, dtm);
			for (int i = 0; i < n; i++) {
				String name = i == getTypeVtableIndex ? "getType" : "vf" + i;
				s.replaceAtOffset(i * 8, ptr(null, dtm), 8, name, hex(progQ(t.vftables.get(0) + 8L * i)));
			}
			vtblTypes.put(t.ti, dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER));
		}
	}

	private void fillStructure(Structure s, TypeRec t, DataTypeManager dtm) {
		Set<String> seen = new HashSet<>();
		int cursor = 0;
		TypeRec sup = t.superTi != 0 ? types.get(t.superTi) : null;
		DataType supDt = sup == null ? null : fbTypes.get(sup.ti);
		if (supDt instanceof Structure && isStructType(sup) && sup.size > 0 && (t.size == 0 || sup.size <= t.size)
			&& sup.ti != t.ti) {
			if (place(s, t, 0, supDt, sup.size, uniqueIdent("super_" + sup.ident, seen, false), null)) {
				cursor = sup.size;
			}
		}
		else if (hasOwnVftablePointer(t)) {
			DataType vtbl = vtblTypes.get(t.ti);
			if (place(s, t, 0, ptr(vtbl, dtm), 8, "__vftable", null)) {
				seen.add("__vftable");
				cursor = 8;
			}
		}
		List<Field> fs = new ArrayList<>(t.fields);
		fs.sort(Comparator.comparingInt(x -> x.offset));
		for (Field fd : fs) {
			if (fd.offset < cursor) {
				count("overlapping/bitfield fields left out of Ghidra structs");
				continue;
			}
			DataType ft = ghidraFieldType(fd, dtm);
			TypeRec ftr = types.get(fd.typeTi);
			String name = uniqueIdent(cIdent(fd.name), seen, false);
			String comment = typeName(fd.typeTi) + ((fd.flags & FIELD_FLAG_SETTING) != 0 ? " (setting)" : "");
			if (ft == null && ftr != null && ftr.size > 0) {
				ft = new ArrayDataType(ByteDataType.dataType, ftr.size, 1, dtm);
			}
			if (ft == null || ft.getLength() <= 0) {
				count("fields of unknown size left undefined");
				continue;
			}
			if (place(s, t, fd.offset, ft, ft.getLength(), name, comment)) {
				cursor = fd.offset + ft.getLength();
			}
		}
	}

	/** Places one component; refuses to grow past the engine's size (that would hide bad data). */
	private boolean place(Structure s, TypeRec t, int off, DataType dt, int len, String name, String comment) {
		try {
			if (off + len > s.getLength()) {
				if (t.size > 0) {
					count("fields past the type's size left out");
					return false;
				}
				s.growStructure(off + len - s.getLength());
			}
			s.replaceAtOffset(off, dt, len, name, comment);
			return true;
		}
		catch (Exception e) {
			count("fields Ghidra refused (" + e.getClass().getSimpleName() + ")");
			return false;
		}
	}

	/** Ghidra type for a field (from the field's TypeInfo), or null if only its size is known. */
	private DataType ghidraFieldType(Field fd, DataTypeManager dtm) {
		TypeRec ft = types.get(fd.typeTi);
		if (ft == null) {
			return fd.isArrayFlag() ? ptr(null, dtm) : null;
		}
		if (ft.category.equals("Array")) {
			// Frostbite Array<T> is a single T* (count stored at data[-4]).
			TypeRec el = ft.elementTi == 0 ? null : types.get(ft.elementTi);
			DataType elDt = el == null || el.category.equals("Array") ? null : valueTypeOf(el, dtm);
			return ptr(elDt, dtm);
		}
		return valueTypeOf(ft, dtm);
	}

	/** The type a value of TypeRec t has when stored in a field or array element. */
	private DataType valueTypeOf(TypeRec t, DataTypeManager dtm) {
		switch (t.category) {
			case "Primitive":
				if (primitiveC(t) != null) {
					return ghidraPrimitive(primitiveName(t), dtm);
				}
				return fbTypes.get(t.ti); // blob struct of the recorded size, or null
			case "Class":
				return ptr(fbTypes.get(t.ti), dtm);
			case "Enum":
			case "ValueType":
				DataType dt = fbTypes.get(t.ti);
				return dt != null && dt.getLength() > 0 ? dt : null;
			case "Array":
				return ptr(null, dtm);
			default:
				return null;
		}
	}

	private DataType ptr(DataType to, DataTypeManager dtm) {
		return new PointerDataType(to, currentProgram.getDefaultPointerSize(), dtm);
	}

	private DataType ghidraPrimitive(String name, DataTypeManager dtm) {
		if (name == null) {
			return null;
		}
		switch (name) {
			case "Boolean":
				return BooleanDataType.dataType;
			case "Int8":
				return SignedByteDataType.dataType;
			case "Uint8":
				return ByteDataType.dataType;
			case "Int16":
				return ShortDataType.dataType;
			case "Uint16":
				return UnsignedShortDataType.dataType;
			case "Int32":
				return IntegerDataType.dataType;
			case "Uint32":
				return UnsignedIntegerDataType.dataType;
			case "Int64":
				return LongLongDataType.dataType;
			case "Uint64":
				return UnsignedLongLongDataType.dataType;
			case "Float32":
				return FloatDataType.dataType;
			case "Float64":
				return DoubleDataType.dataType;
			case "CString":
			case "FileRef":
				return ptr(CharDataType.dataType, dtm);
			case "String":
			case "TypeRef":
				return ptr(null, dtm);
			case "ResourceRef":
				return QWordDataType.dataType;
			default:
				return null;
		}
	}

	/** /fb/reflection: TypeInfo, ClassInfo, *InfoData and FieldInfoData, from the confirmed layout. */
	private void createReflectionTypes(DataTypeManager dtm, CategoryPath cat) {
		DataType charPtr = ptr(CharDataType.dataType, dtm);
		DataType voidPtr = ptr(null, dtm);

		StructureDataType fid = new StructureDataType(cat, "FieldInfoData", FIELD_STRIDE, dtm);
		fid.replaceAtOffset(0x00, charPtr, 8, "name", null);
		fid.replaceAtOffset(0x08, UnsignedShortDataType.dataType, 2, "flags", "0x48 array field, 0x2000 setting");
		fid.replaceAtOffset(0x0A, UnsignedShortDataType.dataType, 2, "offset", null);
		fid.replaceAtOffset(0x0C, UnsignedIntegerDataType.dataType, 4, "unk0C", null);
		fid.replaceAtOffset(0x10, voidPtr, 8, "typeOrValue", "TypeInfo* for fields, value for enum members");
		dtFieldInfoData = dtm.addDataType(fid, DataTypeConflictHandler.REPLACE_HANDLER);
		DataType fidPtr = ptr(dtFieldInfoData, dtm);

		StructureDataType tid = new StructureDataType(cat, "TypeInfoData", 0x20, dtm);
		fillTypeInfoDataHeader(tid, charPtr, voidPtr);
		dtTypeInfoData = dtm.addDataType(tid, DataTypeConflictHandler.REPLACE_HANDLER);

		// Non-class TypeInfos are 0x20 bytes apart on PC.
		// Non-class TypeInfos: 0x20 bytes apart, some only 0x18 (TypeInfoBase).
		StructureDataType tb = new StructureDataType(cat, "TypeInfoBase", 0x18, dtm);
		fillTypeInfoHeader(tb, dtm, voidPtr);
		dtTypeInfoBase = dtm.addDataType(tb, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType ti = new StructureDataType(cat, "TypeInfo", 0x20, dtm);
		fillTypeInfoHeader(ti, dtm, voidPtr);
		ti.replaceAtOffset(0x18, voidPtr, 8, "unk18", null);
		dtTypeInfo = dtm.addDataType(ti, DataTypeConflictHandler.REPLACE_HANDLER);
		DataType tiPtr = ptr(dtTypeInfo, dtm);

		// ClassInfo is 0x50 bytes, or 0x60 (ClassInfoEx) with the +0x50/+0x58 slots.
		StructureDataType ci = new StructureDataType(cat, "ClassInfo", CLASSINFO_SIZE, dtm);
		fillClassInfo(ci, dtm, voidPtr);
		dtClassInfo = dtm.addDataType(ci, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType cx = new StructureDataType(cat, "ClassInfoEx", CLASSINFO_EX_SIZE, dtm);
		fillClassInfo(cx, dtm, voidPtr);
		cx.replaceAtOffset(0x50, voidPtr, 8, "vftableOrClientCreator",
			"data classes: vtable; runtime classes: realm 0 (client) creator");
		cx.replaceAtOffset(0x58, voidPtr, 8, "serverCreator", "runtime classes: realm 1 (server) creator");
		dtClassInfoEx = dtm.addDataType(cx, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType cr = new StructureDataType(cat, "Creator", CREATOR_SIZE, dtm);
		cr.replaceAtOffset(0x00, voidPtr, 8, "vftable", null);
		cr.replaceAtOffset(0x08, voidPtr, 8, "prev", null);
		cr.replaceAtOffset(0x10, voidPtr, 8, "next", "global creator list per realm");
		cr.replaceAtOffset(0x18, voidPtr, 8, "dataType", "TypeInfo* of the data class created from");
		cr.replaceAtOffset(0x20, UnsignedIntegerDataType.dataType, 4, "unk20", null);
		cr.replaceAtOffset(0x24, IntegerDataType.dataType, 4, "realm", "0 client, 1 server");
		cr.replaceAtOffset(0x2C, ByteDataType.dataType, 1, "registered", null);
		dtCreator = dtm.addDataType(cr, DataTypeConflictHandler.REPLACE_HANDLER);

		// 0x38 bytes: on PC the next record starts at +0x38.
		StructureDataType cid = new StructureDataType(cat, "ClassInfoData", 0x38, dtm);
		fillTypeInfoDataHeader(cid, charPtr, voidPtr);
		cid.replaceAtOffset(0x20, tiPtr, 8, "super", null);
		cid.replaceAtOffset(0x28, voidPtr, 8, "create", "create function");
		cid.replaceAtOffset(0x30, fidPtr, 8, "fields", null);
		dtClassInfoData = dtm.addDataType(cid, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType vid = new StructureDataType(cat, "ValueTypeInfoData", 0x30, dtm);
		fillTypeInfoDataHeader(vid, charPtr, voidPtr);
		vid.replaceAtOffset(0x20, voidPtr, 8, "staticInstance", "messages: static instance, vtable at +0");
		vid.replaceAtOffset(0x28, fidPtr, 8, "fields", null);
		dtValueTypeInfoData = dtm.addDataType(vid, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType eid = new StructureDataType(cat, "EnumInfoData", 0x28, dtm);
		fillTypeInfoDataHeader(eid, charPtr, voidPtr);
		eid.replaceAtOffset(0x20, fidPtr, 8, "fields", null);
		dtEnumInfoData = dtm.addDataType(eid, DataTypeConflictHandler.REPLACE_HANDLER);

		StructureDataType aid = new StructureDataType(cat, "ArrayInfoData", 0x28, dtm);
		fillTypeInfoDataHeader(aid, charPtr, voidPtr);
		aid.replaceAtOffset(0x20, tiPtr, 8, "element", null);
		dtArrayInfoData = dtm.addDataType(aid, DataTypeConflictHandler.REPLACE_HANDLER);
	}

	private void fillTypeInfoHeader(StructureDataType s, DataTypeManager dtm, DataType voidPtr) {
		s.replaceAtOffset(0x00, ptr(dtTypeInfoData, dtm), 8, "data", null);
		s.replaceAtOffset(0x08, voidPtr, 8, "next", "next TypeInfo on the global list");
		s.replaceAtOffset(0x10, UnsignedShortDataType.dataType, 2, runtimeIdsValid ? "runtimeId" : "unk10", null);
		s.replaceAtOffset(0x12, UnsignedShortDataType.dataType, 2, "unk12", null);
	}

	private void fillClassInfo(StructureDataType ci, DataTypeManager dtm, DataType voidPtr) {
		fillTypeInfoHeader(ci, dtm, voidPtr);
		ci.replaceAtOffset(0x18, voidPtr, 8, "super", "super ClassInfo");
		ci.replaceAtOffset(0x20, voidPtr, 8, "defaultInstance", "heap; set for data classes only (inferred)");
		boolean v = classTreeValid;
		ci.replaceAtOffset(0x28, UnsignedShortDataType.dataType, 2, v ? "classId" : "unk28",
			v ? "isKindOf(X): X.classId <= classId <= X.lastSubClassId" : null);
		ci.replaceAtOffset(0x2A, UnsignedShortDataType.dataType, 2, v ? "lastSubClassId" : "unk2A", null);
		ci.replaceAtOffset(0x30, voidPtr, 8, v ? "firstDerived" : "unk30", null);
		ci.replaceAtOffset(0x38, voidPtr, 8, v ? "nextSibling" : "unk38", null);
	}

	private static void fillTypeInfoDataHeader(StructureDataType s, DataType charPtr, DataType voidPtr) {
		s.replaceAtOffset(0x00, charPtr, 8, "name", null);
		s.replaceAtOffset(0x08, UnsignedShortDataType.dataType, 2, "flags",
			"bits 0-1 member type, 2-3 category, 4-8 type code");
		s.replaceAtOffset(0x0A, UnsignedShortDataType.dataType, 2, "size", null);
		s.replaceAtOffset(0x10, voidPtr, 8, "module", "ModuleInfo*");
		s.replaceAtOffset(0x18, ByteDataType.dataType, 1, "alignment", null);
		s.replaceAtOffset(0x1A, UnsignedShortDataType.dataType, 2, "fieldCount", null);
	}

	// ------------------------------------------------------------------
	// Labels, applied data, getType / create / vftable
	// ------------------------------------------------------------------

	/** Starts of every record and vftable: applyData never clears across one of these. */
	private TreeSet<Long> guardStarts;

	private void annotateProgram() throws Exception {
		SymbolTable st = currentProgram.getSymbolTable();
		Namespace global = currentProgram.getGlobalNamespace();
		Namespace fbNs = st.getNamespace("fb", global);
		if (fbNs == null) {
			fbNs = st.createNameSpace(global, "fb", SourceType.ANALYSIS);
		}
		removeOwnLabels(st, fbNs);

		guardStarts = new TreeSet<>(knownRecordStarts());
		for (TypeRec t : types.values()) {
			guardStarts.addAll(t.vftables);
			for (long[] c : t.creators) {
				guardStarts.add(c[0]);
			}
			if (t.staticInstance != 0) {
				guardStarts.add(t.staticInstance);
			}
		}
		Map<Long, Integer> vtableUse = vtableFunctionUse();
		boolean thiscall = hasCallingConvention("__thiscall");

		for (TypeRec t : sortedTypes()) {
			if (monitor.isCancelled()) {
				throw new CancelledException();
			}
			// Records
			if (dtTypeInfo != null) {
				applyData(t.ti, runtimeRecordType(t));
				applyData(t.data, infoDataType(t));
				if (t.fieldsPtr != 0 && !t.fields.isEmpty()) {
					applyData(t.fieldsPtr, new ArrayDataType(dtFieldInfoData, t.fields.size(), FIELD_STRIDE,
						currentProgram.getDataTypeManager()));
				}
			}
			// Labels
			label(st, t.ti, "TI_" + t.ident, fbNs);
			label(st, t.data, "TID_" + t.ident, fbNs);
			if (t.fieldsPtr != 0) {
				label(st, t.fieldsPtr, "FID_" + t.ident, fbNs);
			}

			// Methods and vtables
			if (t.getTypeFn == 0 && t.createFn == 0 && t.vftables.isEmpty() && t.typeAccessors.isEmpty()
				&& t.staticInstance == 0 && t.creators.isEmpty()) {
				continue;
			}
			Namespace cls = classNamespace(st, fbNs, t.ident);
			if (cls == null) {
				continue;
			}
			if (t.getTypeFn != 0) {
				Function f = nameFunction(t.getTypeFn, "getType", cls, false);
				setGetTypePrototype(f, t, thiscall);
			}
			for (long a : t.typeAccessors) {
				if (a != t.getTypeFn) {
					nameFunction(a, "typeInfo", cls, false); // not in a vtable: a static accessor
				}
			}
			if (t.createFn != 0) {
				nameFunction(t.createFn, "create", cls, false);
			}
			if (t.staticInstance != 0) {
				label(st, t.staticInstance, "staticInstance", cls);
			}
			for (long[] c : t.creators) {
				applyData(c[0], dtCreator);
				label(st, c[0], "creator_" + realmName(c[1]), cls);
				label(st, readQ(c[0]), "creator_vftable_" + realmName(c[1]), cls);
			}
			for (int i = 0; i < t.vftables.size(); i++) {
				// Several vtables with this type's getType and no create function to tell them
				// apart: subclasses without their own reflection. Keep them as candidates.
				boolean sure = t.vftables.size() == 1 || t.vftableConfirmed;
				String name = t.vftables.size() == 1 ? "vftable"
					: (sure ? "vftable_" : "vftable_candidate_") + (i + 1);
				label(st, t.vftables.get(i), name, cls);
			}
			if (t.vftables.size() == 1) {
				long vt = t.vftables.get(0);
				applyData(vt, vtblTypes.get(t.ti));
				claimVirtualMethods(t, vt, cls, vtableUse, thiscall);
			}
		}
		println("FrostbiteTypeDumper: annotation " + annotateStats);
	}

	/** function -> number of known vtables (of any type) that contain it. */
	private Map<Long, Integer> vtableFunctionUse() {
		Map<Long, Integer> use = new HashMap<>();
		for (Map.Entry<Long, Integer> e : vtableLength.entrySet()) {
			Set<Long> inThis = new HashSet<>();
			for (int i = 0; i < e.getValue(); i++) {
				inThis.add(progQ(e.getKey() + 8L * i));
			}
			for (long fn : inThis) {
				use.merge(fn, 1, Integer::sum);
			}
		}
		return use;
	}

	/**
	 * Moves the functions that occur in this class's vtable and in no other known vtable into
	 * the class namespace (named vf<slot> if they still have a default name) and makes them
	 * __thiscall where the compiler spec has it, so Ghidra types `this` as fb::X*. Functions
	 * shared between vtables are inherited (or folded by the linker) and are left alone.
	 */
	private void claimVirtualMethods(TypeRec t, long vt, Namespace cls, Map<Long, Integer> use, boolean thiscall) {
		Integer n = vtableLength.get(vt);
		if (n == null) {
			return;
		}
		for (int i = 0; i < n; i++) {
			long fn = progQ(vt + 8L * i);
			if (fn == t.getTypeFn || use.getOrDefault(fn, 0) != 1) {
				continue;
			}
			Function f = nameFunction(fn, "vf" + i, cls, true);
			if (f != null && thiscall) {
				try {
					if (f.getSignatureSource() != SourceType.USER_DEFINED && f.getSignatureSource() != SourceType.IMPORTED) {
						f.setCallingConvention("__thiscall");
						count("virtual methods given __thiscall");
					}
				}
				catch (Exception e) {
					count("calling convention refused");
				}
			}
		}
	}

	private boolean hasCallingConvention(String name) {
		try {
			return currentProgram.getCompilerSpec().getCallingConvention(name) != null;
		}
		catch (Exception e) {
			return false;
		}
	}

	/** ClassInfo* getType(fb::X* this). Never touches a signature the user or a PDB set. */
	private void setGetTypePrototype(Function f, TypeRec t, boolean thiscall) {
		if (f == null || f.getSignatureSource() == SourceType.USER_DEFINED || f.getSignatureSource() == SourceType.IMPORTED) {
			return;
		}
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		try {
			DataType self = ptr(fbTypes.get(t.ti), dtm);
			if (thiscall) {
				f.setCallingConvention("__thiscall");
				f.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS, true,
					SourceType.ANALYSIS);
			}
			else {
				f.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.ANALYSIS,
					new ParameterImpl("this", self, currentProgram));
			}
			f.setReturnType(ptr(dtClassInfo, dtm), SourceType.ANALYSIS);
			count("getType prototypes set");
		}
		catch (Exception e) {
			count("getType prototypes refused (" + e.getClass().getSimpleName() + ")");
		}
	}

	/**
	 * Runtime record type that fits before the next known record: ClassInfo is 0x50 or 0x60
	 * bytes (the longer form has the +0x50/+0x58 slots), other TypeInfos 0x18 or 0x20.
	 */
	private DataType runtimeRecordType(TypeRec t) {
		Long n = guardStarts.higher(t.ti);
		long room = n == null ? Long.MAX_VALUE : n - t.ti;
		if (t.category.equals("Class")) {
			return room >= CLASSINFO_EX_SIZE ? dtClassInfoEx : dtClassInfo;
		}
		return room >= 0x20 ? dtTypeInfo : dtTypeInfoBase;
	}

	private DataType infoDataType(TypeRec t) {
		switch (t.category) {
			case "Class":
				return dtClassInfoData;
			case "ValueType":
				return dtValueTypeInfoData;
			case "Enum":
				return dtEnumInfoData;
			case "Array":
			case "FixedArray":
				return t.note.startsWith("inferred array") ? null : dtArrayInfoData;
			default:
				return dtTypeInfoData;
		}
	}

	/** Removes labels this script created on a previous run, so re-runs do not pile up. */
	private void removeOwnLabels(SymbolTable st, Namespace fbNs) {
		List<Symbol> doomed = new ArrayList<>();
		SymbolIterator it = st.getSymbols(fbNs);
		while (it.hasNext()) {
			Symbol s = it.next();
			if (s.getSymbolType() == SymbolType.LABEL && s.getSource() == SourceType.ANALYSIS
				&& (s.getName().startsWith("TI_") || s.getName().startsWith("TID_") || s.getName().startsWith("FID_"))) {
				doomed.add(s);
			}
			else if (s.getSymbolType() == SymbolType.CLASS || s.getSymbolType() == SymbolType.NAMESPACE) {
				Object o = s.getObject();
				if (o instanceof Namespace) {
					SymbolIterator inner = st.getSymbols((Namespace) o);
					while (inner.hasNext()) {
						Symbol c = inner.next();
						if (c.getSymbolType() == SymbolType.LABEL && c.getSource() == SourceType.ANALYSIS
							&& (c.getName().startsWith("vftable") || c.getName().equals("staticInstance")
								|| c.getName().startsWith("creator_"))) {
							doomed.add(c);
						}
					}
				}
			}
		}
		for (Symbol s : doomed) {
			s.delete();
		}
		count("previous labels removed", doomed.size());
	}

	private void label(SymbolTable st, long a, String name, Namespace ns) {
		if (a == 0 || blockAt(a) == null) {
			return;
		}
		Address addr = toAddr(a);
		try {
			// Global TI_/TID_ labels from the old version of this script: removed if this
			// script made them (ANALYSIS) or they are an exact duplicate of the new label.
			// The old version used createLabel(addr, name, false), i.e. USER_DEFINED.
			for (Symbol s : st.getSymbols(addr)) {
				if (s.getSymbolType() != SymbolType.LABEL || !s.getParentNamespace().isGlobal()
					|| s.getSource() == SourceType.IMPORTED) {
					continue;
				}
				boolean ours = s.getSource() == SourceType.ANALYSIS
					&& (s.getName().startsWith("TI_") || s.getName().startsWith("TID_"));
				if (ours || s.getName().equals(name)) {
					s.delete();
				}
			}
			Symbol prim = st.getPrimarySymbol(addr);
			boolean makePrimary = prim == null || prim.getSource() == SourceType.DEFAULT;
			createLabel(addr, name, ns, makePrimary, SourceType.ANALYSIS);
			count("labels");
		}
		catch (Exception e) {
			count("labels refused");
		}
	}

	private Namespace classNamespace(SymbolTable st, Namespace fbNs, String ident) {
		try {
			Namespace ns = st.getNamespace(ident, fbNs);
			return ns != null ? ns : st.createClass(fbNs, ident, SourceType.ANALYSIS);
		}
		catch (Exception e) {
			count("class namespaces refused");
			return null;
		}
	}

	/**
	 * Moves a function into the class namespace and names it, creating the function if needed.
	 * Names from the user or from imported symbols (PDB) are never touched. With onlyIfDefault,
	 * a function that already has an analysis name keeps it (it is still moved into the class).
	 */
	private Function nameFunction(long entry, String name, Namespace cls, boolean onlyIfDefault) {
		try {
			Address a = toAddr(entry);
			Function f = getFunctionAt(a);
			if (f == null) {
				if (currentProgram.getListing().getInstructionAt(a) == null) {
					disassemble(a);
				}
				f = createFunction(a, null);
			}
			if (f == null) {
				count("functions not created");
				return null;
			}
			SourceType src = f.getSymbol().getSource();
			if (src == SourceType.USER_DEFINED || src == SourceType.IMPORTED) {
				count("user/imported function names left alone");
				return f;
			}
			f.setParentNamespace(cls);
			if (!onlyIfDefault || src == SourceType.DEFAULT) {
				f.setName(name, SourceType.ANALYSIS);
			}
			count("functions named");
			return f;
		}
		catch (Exception e) {
			count("functions refused (" + e.getClass().getSimpleName() + ")");
			return null;
		}
	}

	/**
	 * Applies dt at a, clearing conflicting data but never instructions, and never across the
	 * start of another known record or vftable (an over-long record type would otherwise wipe
	 * its neighbour).
	 */
	private void applyData(long a, DataType dt) {
		if (dt == null || a == 0) {
			return;
		}
		int len = dt.getLength();
		if (len <= 0 || blockAt(a) == null || blockAt(a + len - 1) == null) {
			return;
		}
		if (guardStarts != null && !guardStarts.subSet(a + 1, a + len).isEmpty()) {
			count("records not typed (would overlap the next known record)");
			return;
		}
		Address s = toAddr(a);
		Address e = toAddr(a + len - 1);
		Listing listing = currentProgram.getListing();
		try {
			if (listing.getInstructionContaining(s) != null
				|| listing.getInstructions(new AddressSet(s, e), true).hasNext()) {
				count("records not typed (code there)");
				return;
			}
			Data existing = listing.getDefinedDataAt(s);
			if (existing != null && existing.getDataType().isEquivalent(dt)) {
				return;
			}
			clearListing(s, e);
			createData(s, dt);
			count("records typed");
		}
		catch (Exception ex) {
			count("records not typed (" + ex.getClass().getSimpleName() + ")");
		}
	}

	private void count(String what) {
		count(what, 1);
	}

	private void count(String what, int n) {
		annotateStats.merge(what, n, Integer::sum);
	}

	// ------------------------------------------------------------------
	// Memory helpers (all reads go through the emulator state)
	// ------------------------------------------------------------------

	/** Program block containing a, or null. Caches the last hit (most lookups are in one block). */
	private MemoryBlock blockAt(long a) {
		if (a <= 0 || a > 0x0000ffffffffffffL) {
			return null;
		}
		try {
			Address ad = toAddr(a);
			if (lastBlock != null && lastBlock.contains(ad)) {
				return lastBlock;
			}
			MemoryBlock b = mem.getBlock(ad);
			if (b != null) {
				lastBlock = b;
			}
			return b;
		}
		catch (Exception e) {
			return null;
		}
	}

	private boolean isReadable(long a) {
		if (a >= FAKE_HEAP_BASE && a < FAKE_HEAP_END) {
			return true;
		}
		if (a > STACK_TOP - (1 << 20) && a < STACK_TOP) {
			return true;
		}
		return blockAt(a) != null;
	}

	private byte[] readBytes(long a, int n) {
		if (n <= 0 || !isReadable(a)) {
			return new byte[Math.max(n, 0)];
		}
		try {
			return emu.readMemory(toAddr(a), n);
		}
		catch (Exception e) {
			return new byte[n];
		}
	}

	private void writeBytes(long a, byte[] b) {
		if (b.length == 0) {
			return;
		}
		try {
			emu.writeMemory(toAddr(a), b);
		}
		catch (Exception e) {
			// ignore writes to unusable addresses
		}
	}

	private void writeQ(long a, long v) {
		byte[] b = new byte[8];
		for (int i = 0; i < 8; i++) {
			b[i] = (byte) (v >>> (8 * i));
		}
		writeBytes(a, b);
	}

	private long readLE(long a, int n) {
		byte[] b = readBytes(a, n);
		long v = 0;
		for (int i = n - 1; i >= 0; i--) {
			v = (v << 8) | (b[i] & 0xffL);
		}
		return v;
	}

	private long readQ(long a) {
		return readLE(a, 8);
	}

	private long readU32(long a) {
		return readLE(a, 4);
	}

	private int readU16(long a) {
		return (int) readLE(a, 2);
	}

	private int readU8(long a) {
		return (int) readLE(a, 1);
	}

	/** Qword from the program image itself (not the emulator), 0 if unreadable. */
	private long progQ(long a) {
		MemoryBlock b = blockAt(a);
		if (b == null || !b.isInitialized()) {
			return 0;
		}
		byte[] buf = new byte[8];
		try {
			if (mem.getBytes(toAddr(a), buf) != 8) {
				return 0;
			}
		}
		catch (Exception e) {
			return 0;
		}
		return le64(buf, 0);
	}

	private static long le64(byte[] b, int off) {
		long v = 0;
		for (int i = 7; i >= 0; i--) {
			v = (v << 8) | (b[off + i] & 0xffL);
		}
		return v;
	}

	private long reg(String name) {
		BigInteger v = emu.readRegister(name);
		return v == null ? 0 : v.longValue();
	}

	/** Printable, NUL-terminated ASCII string of 1..256 chars, else null. */
	private String readCString(long a) {
		if (!isReadable(a)) {
			return null;
		}
		String s = readCStringRaw(a, 257);
		if (s == null || s.isEmpty() || s.length() > 256) {
			return null;
		}
		return s;
	}

	private String readCStringRaw(long a, int max) {
		StringBuilder sb = new StringBuilder();
		byte[] chunk = readBytes(a, Math.min(max, 64));
		int pos = 0;
		long base = a;
		while (sb.length() < max) {
			if (pos >= chunk.length) {
				base += chunk.length;
				chunk = readBytes(base, 64);
				pos = 0;
			}
			int c = chunk[pos++] & 0xff;
			if (c == 0) {
				return sb.toString();
			}
			if (c < 0x20 || c > 0x7e) {
				return null;
			}
			sb.append((char) c);
		}
		return null;
	}

	// ------------------------------------------------------------------

	private static String hex(long v) {
		return "0x" + Long.toHexString(v);
	}

	private static String pad(String s, int n) {
		StringBuilder sb = new StringBuilder();
		for (int i = s.length(); i < n; i++) {
			sb.append('0');
		}
		return sb.append(s).toString();
	}

	private static String cIdent(String s) {
		StringBuilder sb = new StringBuilder();
		for (char c : s.toCharArray()) {
			sb.append(Character.isLetterOrDigit(c) || c == '_' ? c : '_');
		}
		if (sb.length() == 0 || Character.isDigit(sb.charAt(0))) {
			sb.insert(0, '_');
		}
		return sb.toString();
	}

	private static String js(String s) {
		StringBuilder sb = new StringBuilder("\"");
		for (char c : s.toCharArray()) {
			switch (c) {
				case '"':
					sb.append("\\\"");
					break;
				case '\\':
					sb.append("\\\\");
					break;
				default:
					if (c < 0x20) {
						sb.append(String.format("\\u%04x", (int) c));
					}
					else {
						sb.append(c);
					}
			}
		}
		return sb.append('"').toString();
	}
}

// Frostbite VTable Length Finder for Mirror's Edge Catalyst
//
// Solves the problem of determining exact vtable lengths in Frostbite binaries (where MSVC RTTI is absent).
// Discovers vtable starts from constructor/destructor LEA instructions and object vptr stores, sorts all
// vtable starts in memory order, and determines each vtable's exact length:
//   Length = min(next_vtable_start, first non-code pointer)
//
//@category GridNode
//@author GridNode

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class FrostbiteVTableLengthFinder extends GhidraScript {

    public static class VTableEntry {
        public Address address;
        public int slotCount;
        public Address nextVTable;
        public Address endAddress;
        public boolean isConstructorStore;
        public Set<String> structOffsets = new LinkedHashSet<>();
        public List<Address> codeRefs = new ArrayList<>();
        public List<Long> slots = new ArrayList<>();
    }

    private final Map<Address, VTableEntry> vtables = new HashMap<>();
    private final List<Address> sortedAddrs = new ArrayList<>();

    @Override
    public void run() throws Exception {
        println("==========================================================================");
        println("   Frostbite VTable Length Finder - Mirror's Edge Catalyst");
        println("==========================================================================");

        long t0 = System.currentTimeMillis();
        Memory mem = currentProgram.getMemory();

        // 1. Scan instructions for LEA reg, [rip + disp] pointing to data sections
        println("Scanning instructions for constructor / destructor vtable references...");
        InstructionIterator iter = currentProgram.getListing().getInstructions(true);
        int insnCount = 0;
        int leaCount = 0;

        while (iter.hasNext()) {
            Instruction insn = iter.next();
            insnCount++;
            if (!insn.getMnemonicString().equalsIgnoreCase("LEA")) {
                continue;
            }
            leaCount++;

            Reference[] refs = insn.getReferencesFrom();
            for (Reference ref : refs) {
                Address toAddr = ref.getToAddress();
                if (toAddr == null) continue;
                long offset = toAddr.getOffset();
                if ((offset & 7) != 0) continue; // vtables are 8-byte aligned

                MemoryBlock targetBlock = mem.getBlock(toAddr);
                if (targetBlock == null || targetBlock.isExecute() || !targetBlock.isRead()) {
                    continue; // Must be in read-only / data block
                }

                // Verify slot 0 points to executable code
                try {
                    long firstPtr = mem.getLong(toAddr);
                    Address firstPtrAddr = currentProgram.getAddressFactory().getAddress(String.format("0x%x", firstPtr));
                    MemoryBlock codeBlock = mem.getBlock(firstPtrAddr);
                    if (codeBlock == null || !codeBlock.isExecute()) {
                        continue;
                    }
                } catch (Exception e) {
                    continue;
                }

                // Record vtable candidate
                VTableEntry entry = vtables.computeIfAbsent(toAddr, k -> {
                    VTableEntry v = new VTableEntry();
                    v.address = toAddr;
                    return v;
                });
                entry.codeRefs.add(insn.getAddress());

                // Detect constructor store pattern: MOV [reg + offset], leaReg
                String leaReg = insn.getDefaultOperandRepresentation(0);
                Instruction scan = insn.getNext();
                for (int step = 0; step < 4 && scan != null; step++) {
                    if (scan.getMnemonicString().equalsIgnoreCase("MOV") && scan.getNumOperands() == 2) {
                        String op0 = scan.getDefaultOperandRepresentation(0);
                        String op1 = scan.getDefaultOperandRepresentation(1);
                        if (op1.equalsIgnoreCase(leaReg) && op0.contains("[")) {
                            entry.isConstructorStore = true;
                            entry.structOffsets.add(op0);
                            break;
                        }
                    }
                    scan = scan.getNext();
                }
            }
        }

        long t1 = System.currentTimeMillis();
        println(String.format("Scan finished in %d ms (%d instructions, %d LEAs). Found %d vtables.",
                (t1 - t0), insnCount, leaCount, vtables.size()));

        // 2. Sort addresses
        sortedAddrs.addAll(vtables.keySet());
        Collections.sort(sortedAddrs);

        // 3. Compute exact lengths
        println("Computing exact slot counts bounded by next vtable and non-code data...");
        for (int i = 0; i < sortedAddrs.size(); i++) {
            Address curAddr = sortedAddrs.get(i);
            VTableEntry entry = vtables.get(curAddr);
            Address nextAddr = (i + 1 < sortedAddrs.size()) ? sortedAddrs.get(i + 1) : null;
            entry.nextVTable = nextAddr;

            int slots = 0;
            Address slotAddr = curAddr;
            while (true) {
                if (nextAddr != null && slotAddr.compareTo(nextAddr) >= 0) {
                    break; // Reached start of next vtable
                }
                try {
                    long fnPtr = mem.getLong(slotAddr);
                    Address fnAddr = currentProgram.getAddressFactory().getAddress(String.format("0x%x", fnPtr));
                    MemoryBlock blk = mem.getBlock(fnAddr);
                    if (blk == null || !blk.isExecute()) {
                        break; // Non-code pointer
                    }
                    entry.slots.add(fnPtr);
                    slots++;
                    slotAddr = slotAddr.add(8);
                } catch (Exception e) {
                    break;
                }
            }
            entry.slotCount = slots;
            entry.endAddress = curAddr.add(8L * slots);
        }

        long t2 = System.currentTimeMillis();

        // 4. Print key class analysis
        printKeyClasses();

        // 5. Handle script arguments (e.g. "export FILE" or "label")
        String[] args = getScriptArgs();
        if (args != null && args.length > 0) {
            if (args[0].equalsIgnoreCase("export") && args.length > 1) {
                exportToJson(new File(args[1]));
            } else if (args[0].equalsIgnoreCase("label")) {
                labelAllVTables();
            }
        }

        println(String.format("\nCompleted in %d ms total.", (t2 - t0)));
        println("==========================================================================");
    }

    private void printKeyClasses() {
        println("\n--------------------------------------------------------------------------");
        println("KEY VTABLE INSPECTION:");
        println("--------------------------------------------------------------------------");

        // Faith ViewMode
        Address faithAddr = toAddr(0x141c3cbf0L);
        VTableEntry faith = vtables.get(faithAddr);
        if (faith != null) {
            println("[ClientCharacter_ViewModeInterface (Faith Body Hook)]");
            println("  VTable Start:    " + faith.address);
            println("  Slot Count:      " + faith.slotCount + " slots (" + (faith.slotCount * 8) + " bytes)");
            println("  Next VTable:     " + faith.nextVTable);
            println("  End Address:     " + faith.endAddress);
            println("  Struct Offsets:  " + faith.structOffsets);
            println("  Code Sites:      " + faith.codeRefs);
            println("  Faith Slot 15:   0x141c3cc68 -> GetViewMode (0x14390ed90)");
        }

        // ClientCharacter primary and sub-vtables
        long[] ccVts = {0x141c3c760L, 0x141c3cb58L, 0x141c3cb78L, 0x141c3cbd0L, 0x141c3cbf0L, 0x141c3ccc8L};
        String[] ccDesc = {"+0x00 Primary", "+0x68 Interface", "+0x70 Interface", "+0xc8 Interface", "+0x1d8 ViewModeInterface", "+0x2b0 Interface"};
        println("\n[ClientCharacter Complete VTable Hierarchy]");
        for (int i = 0; i < ccVts.length; i++) {
            Address a = toAddr(ccVts[i]);
            VTableEntry e = vtables.get(a);
            if (e != null) {
                println(String.format("  0x%x (%-25s): %3d slots (%4d bytes), next: %s, store: %s",
                        e.address.getOffset(), ccDesc[i], e.slotCount, e.slotCount * 8, e.nextVTable, e.structOffsets));
            }
        }

        // Camera classes
        println("\n[Camera VTables]");
        long[] camVts = {0x141b18e58L, 0x141b37ad0L, 0x141b3b2b0L};
        String[] camDesc = {"FPSCamera", "CameraBase", "CameraObject"};
        for (int i = 0; i < camVts.length; i++) {
            Address a = toAddr(camVts[i]);
            VTableEntry e = vtables.get(a);
            if (e != null) {
                println(String.format("  0x%x (%-15s): %3d slots (%4d bytes), next: %s, store: %s",
                        e.address.getOffset(), camDesc[i], e.slotCount, e.slotCount * 8, e.nextVTable, e.structOffsets));
            }
        }
    }

    private void exportToJson(File file) {
        println("Exporting " + sortedAddrs.size() + " vtables to JSON: " + file.getAbsolutePath());
        try (PrintWriter writer = new PrintWriter(new FileWriter(file))) {
            JsonArray root = new JsonArray();
            for (Address a : sortedAddrs) {
                VTableEntry e = vtables.get(a);
                JsonObject obj = new JsonObject();
                obj.addProperty("address", String.format("0x%x", e.address.getOffset()));
                obj.addProperty("slot_count", e.slotCount);
                obj.addProperty("size_bytes", e.slotCount * 8);
                obj.addProperty("next_vtable", e.nextVTable != null ? String.format("0x%x", e.nextVTable.getOffset()) : null);
                obj.addProperty("end_address", String.format("0x%x", e.endAddress.getOffset()));
                obj.addProperty("is_constructor_store", e.isConstructorStore);

                JsonArray offsets = new JsonArray();
                for (String off : e.structOffsets) offsets.add(off);
                obj.add("struct_offsets", offsets);

                JsonArray refs = new JsonArray();
                for (Address r : e.codeRefs) refs.add(String.format("0x%x", r.getOffset()));
                obj.add("code_refs", refs);

                root.add(obj);
            }
            Gson gson = new GsonBuilder().setPrettyPrinting().create();
            writer.write(gson.toJson(root));
            println("Export complete. File size: " + file.length() + " bytes.");
        } catch (Exception ex) {
            println("Failed to export JSON: " + ex.getMessage());
        }
    }

    private void labelAllVTables() {
        println("Creating labels in Ghidra for all " + sortedAddrs.size() + " vtables...");
        int count = 0;
        for (Address a : sortedAddrs) {
            VTableEntry e = vtables.get(a);
            try {
                Symbol existing = currentProgram.getSymbolTable().getPrimarySymbol(a);
                if (existing == null || existing.getSource() == SourceType.DEFAULT) {
                    String name = String.format("vftable_%08x", a.getOffset());
                    currentProgram.getSymbolTable().createLabel(a, name, SourceType.ANALYSIS);
                    count++;
                }
            } catch (Exception ex) {
                // Ignore symbol collision
            }
        }
        println("Labeled " + count + " vtables with symbols ending in 'vftable'.");
    }
}

// GridNodeSync: GridNode's annotations <-> this Ghidra program (Mirror's Edge Catalyst, PC).
//
// Import (GridNode -> Ghidra)
//   python tools/gen/gridnode.py ghidra-export          writes build/gridnode-ghidra.json
//   then run this script, choose Import and pick that file. It
//     - creates or replaces a struct per annotated class and struct in /GridNode, with the
//       annotated members (types, names, comments with confidence and source);
//     - names annotated functions, virtual functions (Class::name), globals, code sites, vtables
//       (Class::vftable) and hook slots, and puts GridNode's note in a plate / EOL comment.
//   Names you gave yourself (USER_DEFINED) are never changed; GridNode's names are ANALYSIS, so a
//   re-import replaces its own and nothing else.
//
// Export (Ghidra -> GridNode)
//   run this script, choose Export and save the file, then
//   python tools/gen/gridnode.py ghidra-import that-file   writes db/ghidra-inbox.yaml
//   It carries the functions and labels you named (USER_DEFINED), with the prototype when you set
//   it, and the members you named in /GridNode structs. GridNode marks them confidence inferred,
//   source ghidra, for review.
//
// Export before importing again if you edited the /GridNode structs: an import replaces them.
//
// Friends: their export goes through `gridnode.py ghidra-import THEIR-FILE --who NAME` (annotations to
// review) or `gridnode.py ghidra-relay THEIR-FILE --who NAME` (a file this script imports straight
// into your Ghidra: their names where you have none of your own, their comments in a block of their own).
//
// Recordings (GridNode menu -> Recorder): `python tools/recording/summarize.py REC --ghidra OUT --exe DUMP`
// writes a file this script imports the same way: functions made where Ghidra has none around a
// recorded frame, a bookmark (category "GridNode recording") at each call site with what came through
// it, and a "GridNode [recording] (REC)" comment block per function with counts.
//
// Without dialogs (headless, or run remotely): script arguments "import FILE" or "export FILE".
//
// Annotated signatures become prototypes (ANALYSIS; never over one you set).
//
// Classes with a vtable (annotated, from the dump, or asked for with `ghidra-export --class NAME`) also
// get <Class>_vtbl in /GridNode/vtables, named and typed from what each slot points to; the class struct
// gets `vtbl` at +0, so decompiled calls read this->vtbl->name(...).
//
// Needs Ghidra 12 (CommentType); run on 12.1.4 (written against 12.0.3).
//
//@category GridNode
//@author GridNode

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.Reader;
import java.io.Writer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Set;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.FunctionRenameOption;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.BooleanDataType;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.ParameterDefinitionImpl;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.LongLongDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.ShortDataType;
import ghidra.program.model.data.SignedByteDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.Undefined8DataType;
import ghidra.program.model.data.UnsignedCharDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.UnsignedLongLongDataType;
import ghidra.program.model.data.UnsignedShortDataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.BookmarkManager;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.CommentType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolType;

public class GridNodeSync extends GhidraScript {

	private static final String IN_FORMAT = "gridnode-ghidra";
	private static final String OUT_FORMAT = "gridnode-ghidra-export";
	private static final CategoryPath CATEGORY = new CategoryPath("/GridNode");
	private static final CategoryPath VT_CATEGORY = new CategoryPath("/GridNode/vtables");
	private static final CategoryPath VT_FUNCTIONS = new CategoryPath(VT_CATEGORY, "functions");
	private static final String MARK = "GridNode [";

	private DataTypeManager dtm;
	private int named, kept, structs, comments, failed, vtables, thisTyped, protos, protosKept;
	private final Set<Address> thisSeen = new HashSet<>(); // functions whose `this` this import has decided
	private DecompInterface decomp;                         // made on first use, disposed after the import

	@Override
	protected void run() throws Exception {
		dtm = currentProgram.getDataTypeManager();
		String importChoice = "Import GridNode's annotations into Ghidra";
		String exportChoice = "Export the names I gave in Ghidra, for GridNode";
		// Script arguments (headless, or a remote run): "import FILE" or "export FILE". No dialogs then.
		String[] args = getScriptArgs();
		if (args != null && args.length >= 2) {
			// The rest is one path, joined back with spaces: GhidraMCP splits its argument string on
			// whitespace with no quoting, so "D:\My Files\x.json" arrives as two arguments. (A path with
			// two spaces in a row would come back with one; none of ours have that.)
			File f = new File(String.join(" ", Arrays.copyOfRange(args, 1, args.length)));
			if (args[0].equalsIgnoreCase("import")) {
				doImport(f);
			}
			else if (args[0].equalsIgnoreCase("export")) {
				doExport(f);
			}
			else {
				printerr("GridNodeSync arguments: import FILE | export FILE");
			}
			return;
		}
		String choice = askChoice("GridNodeSync", "What should GridNodeSync do?", Arrays.asList(importChoice, exportChoice),
			importChoice);
		if (choice.equals(importChoice)) {
			File f = askFile("GridNode export (build/gridnode-ghidra.json)", "Import");
			doImport(f);
		}
		else {
			File f = askFile("Where to save the export (e.g. ghidra-names.json)", "Save");
			doExport(f);
		}
	}

	// ---- import ------------------------------------------------------------------------------------
	private void doImport(File file) throws Exception {
		JsonObject doc;
		try (Reader r = new FileReader(file)) {
			doc = new Gson().fromJson(r, JsonObject.class);
		}
		if (!IN_FORMAT.equals(str(doc, "format")) || doc.get("version").getAsInt() != 1) {
			printerr("Not a GridNode export (gridnode.py ghidra-export): " + file);
			return;
		}
		JsonArray ss = array(doc, "structs");
		// Pass 1: every struct exists (members may point at each other); pass 2: members.
		for (JsonElement e : ss) {
			JsonObject s = e.getAsJsonObject();
			StructureDataType st = new StructureDataType(CATEGORY, str(s, "name"), s.get("size").getAsInt(), dtm);
			st.setDescription(str(s, "comment"));
			dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER);
		}
		for (JsonElement e : ss) {
			JsonObject s = e.getAsJsonObject();
			Structure st = (Structure) dtm.getDataType(CATEGORY, str(s, "name"));
			if (st == null) {
				continue;
			}
			for (JsonElement me : s.getAsJsonArray("members")) {
				JsonObject m = me.getAsJsonObject();
				int off = m.get("offset").getAsInt();
				DataType dt = resolve(str(m, "type"));
				int len = dt.getLength() > 0 ? dt.getLength() : 8;
				try {
					if (off + len > st.getLength()) {
						st.growStructure(off + len - st.getLength());
					}
					st.replaceAtOffset(off, dt, len, str(m, "name"), str(m, "comment"));
				}
				catch (Exception ex) {
					failed++;
					println("struct " + st.getName() + " member " + str(m, "name") + " at 0x" + Integer.toHexString(off) +
						": " + ex.getMessage());
				}
			}
			structs++;
		}
		for (JsonElement e : array(doc, "functions")) {
			JsonObject f = e.getAsJsonObject();
			nameFunction(addr(f), str(f, "name"), str(f, "comment"));
			if (f.has("proto") && f.get("proto").isJsonObject()) {
				applyPrototype(addr(f), f.getAsJsonObject("proto"));
			}
		}
		for (JsonElement e : array(doc, "labels")) {
			JsonObject l = e.getAsJsonObject();
			nameLabel(addr(l), str(l, "name"), str(l, "type"), str(l, "comment"));
		}
		// Vtable types, after the names above so slots are named from them: each class struct with a
		// vtable gets <Class>_vtbl in /GridNode/vtables and a `vtbl` pointer at +0.
		List<JsonObject> withVtable = new ArrayList<>();
		for (JsonElement e : ss) {
			JsonObject s = e.getAsJsonObject();
			if (s.has("vtable") && !s.get("vtable").isJsonNull()) {
				withVtable.add(s);
			}
		}
		withVtable.sort((x, y) -> Integer.compare(x.has("depth") ? x.get("depth").getAsInt() : 0,
			y.has("depth") ? y.get("depth").getAsInt() : 0)); // base classes first
		try {
			for (JsonObject s : withVtable) {
				applyVtable(str(s, "name"), s.get("vtable").getAsLong());
			}
		}
		finally {
			if (decomp != null) {
				decomp.dispose();
				decomp = null;
			}
		}
		// From recordings (summarize.py --ghidra): functions where Ghidra has none, comments, bookmarks.
		int made = 0, marks = 0;
		for (JsonElement e : array(doc, "createFunctions")) {
			JsonObject c = e.getAsJsonObject();
			Address start = addr(c);
			Address at = c.has("at") ? toAddr(c.get("at").getAsLong()) : start;
			if (getFunctionContaining(at) != null || getFunctionAt(start) != null) {
				continue;
			}
			try {
				disassemble(start);
				if (createFunction(start, null) != null) {
					made++;
				}
				else {
					failed++;
					println("no function could be made at " + start + " (frame " + at + ")");
				}
			}
			catch (Exception ex) {
				failed++;
				println("function at " + start + ": " + ex.getMessage());
			}
		}
		for (JsonElement e : array(doc, "comments")) {
			JsonObject c = e.getAsJsonObject();
			setComment(addr(c), "eol".equals(str(c, "kind")) ? CommentType.EOL : CommentType.PLATE, str(c, "comment"));
		}
		BookmarkManager bm = currentProgram.getBookmarkManager();
		for (JsonElement e : array(doc, "bookmarks")) {
			JsonObject b = e.getAsJsonObject();
			Address a = addr(b);
			if (b.has("afterCall") && b.get("afterCall").getAsBoolean()) {
				// A return address: mark the call instruction before it.
				Instruction prev = getInstructionBefore(a);
				if (prev != null && prev.getFlowType().isCall()) {
					a = prev.getAddress();
				}
			}
			// One bookmark per address and category: a re-import replaces it.
			bm.setBookmark(a, BookmarkType.NOTE, str(b, "category"), str(b, "comment"));
			marks++;
		}
		println(String.format("GridNodeSync import: %d structs, %d names set, %d kept (yours), %d comments, " +
			"%d functions made, %d bookmarks, %d prototypes (%d kept: yours), %d vtables (%d this types), %d problems", structs,
			named, kept, comments, made, marks, protos, protosKept, vtables, thisTyped, failed));
	}

	// <Class>_vtbl: one pointer per slot, from the vtable's start until a slot no longer points at code
	// (or the next class's vftable label). A slot is named after the function it points to when that
	// has a name (yours, GridNode's, or Class::name for annotated virtuals), else vf_0xNN; it is typed
	// with that function's prototype when one has been set (so calls through it keep their
	// arguments), else void*. The vtable itself is typed with the struct, and the class struct gets
	// `vtbl` at +0 unless something is annotated there.
	private void applyVtable(String cls, long va) {
		try {
			Address a = toAddr(va);
			Memory mem = currentProgram.getMemory();
			StructureDataType vt = new StructureDataType(VT_CATEGORY, cls + "_vtbl", 0, dtm);
			vt.setDescription("Vtable of " + cls + " at " + a + " (GridNodeSync)");
			Set<String> used = new HashSet<>();
			int n = 0;
			for (; n < 1024; n++) {
				Address slot = a.add(8L * n);
				if (n > 0) {
					Symbol sym = currentProgram.getSymbolTable().getPrimarySymbol(slot);
					if (sym != null && sym.getName().endsWith("vftable")) {
						break; // the next class's vtable starts here
					}
				}
				Address target;
				try {
					target = toAddr(mem.getLong(slot));
				}
				catch (Exception ex) {
					break;
				}
				MemoryBlock block = mem.getBlock(target);
				if (block == null || !block.isExecute()) {
					break;
				}
				Function f = getFunctionAt(target);
				if (f != null && thisSeen.add(f.getEntryPoint())) {
					typeThis(f, cls);
				}
				String field = slotName(f, n);
				if (!used.add(field)) {
					field = field + String.format("_0x%x", 8 * n);
					used.add(field);
				}
				DataType ft;
				if (f != null && f.getSignatureSource() != SourceType.DEFAULT) {
					ft = new PointerDataType(
						new FunctionDefinitionDataType(VT_FUNCTIONS, cls + "_" + field, f.getSignature(), dtm), 8, dtm);
				}
				else {
					ft = new PointerDataType(VoidDataType.dataType, 8, dtm);
				}
				vt.add(ft, 8, field, String.format("+0x%x: %s%s", 8 * n, target, f == null ? "" : " " + f.getName(true)));
			}
			if (n == 0) {
				failed++;
				println("vtable " + cls + " at " + a + ": its first slot does not point at code");
				return;
			}
			DataType resolved = dtm.addDataType(vt, DataTypeConflictHandler.REPLACE_HANDLER);
			Structure cs = (Structure) dtm.getDataType(CATEGORY, cls);
			if (cs != null) {
				DataTypeComponent c0 = cs.getComponentContaining(0);
				if (c0 == null || c0.getFieldName() == null || c0.getFieldName().equals("vtbl")) {
					if (cs.getLength() < 8) {
						cs.growStructure(8 - cs.getLength());
					}
					cs.replaceAtOffset(0, new PointerDataType(resolved, 8, dtm), 8, "vtbl", cls + "_vtbl");
				}
			}
			clearListing(a, a.add(8L * n - 1));
			createData(a, resolved);
			vtables++;
		}
		catch (Exception ex) {
			failed++;
			println("vtable " + cls + ": " + ex.getMessage());
		}
	}

	// An annotated signature as the function's prototype (ANALYSIS), so calls to it decompile with its
	// parameter names and types. Never over a prototype you set (USER_DEFINED / IMPORTED). A virtual's
	// class comes first, as `self` (a pointer to its /GridNode struct); the vtable pass then leaves it.
	private void applyPrototype(Address a, JsonObject proto) {
		Function f = getFunctionAt(a);
		if (f == null) {
			return;
		}
		SourceType src = f.getSignatureSource();
		if (src == SourceType.USER_DEFINED || src == SourceType.IMPORTED) {
			protosKept++;
			return;
		}
		try {
			FunctionDefinitionDataType fd = new FunctionDefinitionDataType(f.getName(), dtm);
			fd.setReturnType(resolve(str(proto, "returns")));
			List<ParameterDefinition> ps = new ArrayList<>();
			Set<String> names = new HashSet<>();
			String cls = str(proto, "this");
			if (!cls.isEmpty()) {
				DataType cs = dtm.getDataType(CATEGORY, cls);
				ps.add(new ParameterDefinitionImpl("self",
					new PointerDataType(cs != null ? cs : VoidDataType.dataType, 8, dtm), "this (" + cls + ")"));
				names.add("self");
			}
			int i = 0;
			for (JsonElement pe : array(proto, "params")) {
				JsonObject p = pe.getAsJsonObject();
				String n = str(p, "name");
				if (n.isEmpty() || n.equals("this") || !names.add(n)) {
					n = "a" + i;
					names.add(n);
				}
				ps.add(new ParameterDefinitionImpl(n, resolve(str(p, "type")), null));
				i++;
			}
			fd.setArguments(ps.toArray(new ParameterDefinition[0]));
			ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(a, fd, SourceType.ANALYSIS, false,
				FunctionRenameOption.NO_CHANGE);
			if (cmd.applyTo(currentProgram, monitor)) {
				protos++;
				if (!cls.isEmpty()) {
					thisSeen.add(a); // annotated: the vtable pass keeps this class as `this`
				}
			}
			else {
				failed++;
				println("prototype of " + f.getName() + ": " + cmd.getStatusMsg());
			}
		}
		catch (Exception ex) {
			failed++;
			println("prototype of " + f.getName() + ": " + ex.getMessage());
		}
	}

	// The first parameter of a class's virtual function becomes a pointer to the class struct, so the
	// decompiler shows this->member and this->vtbl->name(...). Never a prototype you set (USER_DEFINED /
	// IMPORTED). Most functions have no stored parameters (the decompiler infers them each time), so the
	// decompiler's own parameters are committed first (ANALYSIS: nothing added or dropped); a __thiscall
	// function becomes __fastcall first (the same registers on x64), since its automatic `this` cannot be
	// retyped. Only a generic type is replaced (undefined, an 8-byte integer, a pointer to nothing in
	// particular). Tiny stubs are left alone. Classes are done base first, so a function several classes
	// share gets the most basic one.
	private void typeThis(Function f, String cls) {
		try {
			SourceType src = f.getSignatureSource();
			if (src == SourceType.USER_DEFINED || src == SourceType.IMPORTED || f.getBody().getNumAddresses() < 16) {
				return;
			}
			Structure cs = (Structure) dtm.getDataType(CATEGORY, cls);
			if (cs == null) {
				return;
			}
			if ("__thiscall".equals(f.getCallingConventionName())) {
				f.setCallingConvention("__fastcall");
			}
			if (f.getParameterCount() < 1) {
				if (decomp == null) {
					decomp = new DecompInterface();
					decomp.openProgram(currentProgram);
				}
				DecompileResults r = decomp.decompileFunction(f, 30, monitor);
				if (r == null || !r.decompileCompleted() || r.getHighFunction() == null) {
					return;
				}
				HighFunctionDBUtil.commitParamsToDatabase(r.getHighFunction(), true,
					HighFunctionDBUtil.ReturnCommitOption.NO_COMMIT, SourceType.ANALYSIS);
				if (f.getParameterCount() < 1) {
					return;
				}
			}
			Parameter p0 = f.getParameter(0);
			if (p0.isAutoParameter()) {
				return;
			}
			if (p0.getSource() == SourceType.USER_DEFINED || !genericThis(p0.getDataType())) {
				return;
			}
			// A class GridNode chose before is only replaced by a more basic one (an import that knows
			// fewer classes, such as a recording's, must not push a shared function down to a subclass).
			String earlier = gridNodeClass(p0.getDataType());
			if (earlier != null && !earlier.equals(cls) && !basesOf(earlier).contains(cls)) {
				return;
			}
			p0.setDataType(new PointerDataType(cs, 8, dtm), SourceType.ANALYSIS);
			thisTyped++;
		}
		catch (Exception ex) {
			println("this of " + f.getName() + ": " + ex.getMessage());
		}
	}

	// The /GridNode class a pointer points to, or null.
	private static String gridNodeClass(DataType dt) {
		if (dt instanceof Pointer) {
			DataType to = ((Pointer) dt).getDataType();
			if (to instanceof Structure && to.getCategoryPath().equals(CATEGORY)) {
				return to.getName();
			}
		}
		return null;
	}

	// A class struct's base classes, from its description ("Bases: A <- B <- C", written by ghidra-export).
	private List<String> basesOf(String cls) {
		List<String> out = new ArrayList<>();
		DataType dt = dtm.getDataType(CATEGORY, cls);
		String d = dt != null ? dt.getDescription() : null;
		int at = d != null ? d.indexOf("Bases: ") : -1;
		if (at >= 0) {
			for (String b : d.substring(at + 7).split("\n", 2)[0].split(" <- ")) {
				out.add(b.trim());
			}
		}
		return out;
	}

	private static boolean genericThis(DataType dt) {
		if (dt == null || Undefined.isUndefined(dt)) {
			return true;
		}
		String n = dt.getName();
		if (dt.getLength() == 8 && (n.equals("longlong") || n.equals("ulonglong") || n.equals("qword"))) {
			return true;
		}
		if (dt instanceof Pointer) {
			DataType to = ((Pointer) dt).getDataType();
			if (to != null && to.getCategoryPath().isAncestorOrSelf(CATEGORY)) {
				return true; // a class GridNode chose before (an import re-decides it, base classes first)
			}
			return to == null || Undefined.isUndefined(to) || to instanceof VoidDataType || to.getLength() == 8 &&
				(to.getName().equals("longlong") || to.getName().equals("ulonglong") || Undefined.isUndefined(to));
		}
		return false;
	}

	// A tiny shared stub (emptyStub, returnFalse) is a base default that subclasses override: naming the
	// slot after it would mislead, so those slots stay vf_0xNN (the field comment still names the target).
	private String slotName(Function f, int n) {
		if (f == null || f.getSymbol().getSource() == SourceType.DEFAULT || f.getBody().getNumAddresses() < 16) {
			return String.format("vf_0x%x", 8 * n);
		}
		String name = f.getName().replaceAll("[^A-Za-z0-9_]", "_");
		return name.isEmpty() || Character.isDigit(name.charAt(0)) ? "_" + name : name;
	}

	private void nameFunction(Address a, String name, String comment) {
		try {
			Function f = getFunctionAt(a);
			if (f == null) {
				f = createFunction(a, null);
			}
			if (f == null) {
				failed++;
				println("no function could be made at " + a + " for " + name);
				return;
			}
			if (f.getSymbol().getSource() == SourceType.USER_DEFINED) {
				kept++;
			}
			else {
				setQualifiedName(f.getSymbol(), name);
				named++;
			}
			setComment(a, CommentType.PLATE, comment);
		}
		catch (Exception ex) {
			failed++;
			println("function " + name + " at " + a + ": " + ex.getMessage());
		}
	}

	private void nameLabel(Address a, String name, String type, String comment) {
		try {
			Symbol primary = currentProgram.getSymbolTable().getPrimarySymbol(a);
			if (primary != null && primary.getSource() == SourceType.USER_DEFINED) {
				kept++;
			}
			else {
				Namespace ns = namespaceFor(name);
				String simple = simpleName(name);
				Symbol s = currentProgram.getSymbolTable().createLabel(a, simple, ns, SourceType.ANALYSIS);
				s.setPrimary();
				named++;
			}
			if (type != null && !type.isEmpty() && getFunctionContaining(a) == null) {
				DataType dt = resolve(type);
				Data d = getDataAt(a);
				if (dt.getLength() > 0 && (d == null || !d.isDefined())) {
					try {
						createData(a, dt);
					}
					catch (Exception ex) {
						// something else is defined there: leave the bytes as they are
					}
				}
			}
			setComment(a, getFunctionContaining(a) != null ? CommentType.EOL : CommentType.PLATE, comment);
		}
		catch (Exception ex) {
			failed++;
			println("label " + name + " at " + a + ": " + ex.getMessage());
		}
	}

	// A comment is your own text, then blocks that start with a "GridNode [" line: GridNode's own, and one
	// per friend relayed (gridnode.py ghidra-relay, "GridNode [relay] (whose)"). An import replaces only the
	// block it brings (same first line up to the closing parenthesis); your text and other blocks stay.
	private void setComment(Address a, CommentType kind, String comment) {
		if (comment == null || comment.isEmpty()) {
			return;
		}
		CodeUnit cu = currentProgram.getListing().getCodeUnitAt(a);
		if (cu == null) {
			return;
		}
		String old = cu.getComment(kind);
		String key = blockKey(comment);
		// Your text first, then one block per source in a fixed order (GridNode's, then relays and
		// recordings by name), so importing again in any order leaves the comment as it was.
		List<String> all = new ArrayList<>();
		for (String block : blocks(old)) {
			if (!blockKey(block).equals(key)) {
				all.add(block);
			}
		}
		all.add(comment.trim());
		all.sort((x, y) -> {
			String kx = blockKey(x), ky = blockKey(y);
			if (kx.equals(ky)) {
				return 0;
			}
			return kx.equals("gridnode") ? -1 : ky.equals("gridnode") ? 1 : kx.compareTo(ky);
		});
		StringBuilder kept = new StringBuilder(ownText(old));
		for (String block : all) {
			kept.append(kept.length() > 0 ? "\n" : "").append(block);
		}
		String text = kept.toString();
		if (!text.equals(old)) {
			cu.setComment(kind, text);
			comments++;
		}
	}

	// The text before the first block (what you wrote).
	private static String ownText(String c) {
		if (c == null) {
			return "";
		}
		int at = c.startsWith(MARK) ? 0 : c.indexOf("\n" + MARK);
		return (at < 0 ? c : c.substring(0, at)).trim();
	}

	private static List<String> blocks(String c) {
		List<String> out = new ArrayList<>();
		if (c == null) {
			return out;
		}
		int at = c.startsWith(MARK) ? 0 : c.indexOf("\n" + MARK);
		while (at >= 0) {
			int start = c.charAt(at) == '\n' ? at + 1 : at;
			int next = c.indexOf("\n" + MARK, start);
			out.add(c.substring(start, next < 0 ? c.length() : next).trim());
			at = next;
		}
		return out;
	}

	// "GridNode [relay] (Alex's Ghidra)" -> "relay (Alex's Ghidra)"; "GridNode [recording] (rec.jsonl)" ->
	// "recording (rec.jsonl)"; any other GridNode block (annotations) -> "gridnode".
	private static String blockKey(String block) {
		String first = block.split("\n", 2)[0];
		for (String kind : new String[] { "relay", "recording" }) {
			if (first.startsWith(MARK + kind + "]")) {
				int close = first.indexOf(')');
				int from = MARK.length() + kind.length() + 1;
				return kind + " " + (close >= from ? first.substring(from, close + 1).trim() : "");
			}
		}
		return "gridnode";
	}

	private void setQualifiedName(Symbol s, String name) throws Exception {
		s.setNameAndNamespace(simpleName(name), namespaceFor(name), SourceType.ANALYSIS);
	}

	private String simpleName(String name) {
		int at = name.lastIndexOf("::");
		return at >= 0 ? name.substring(at + 2) : name;
	}

	private Namespace namespaceFor(String name) throws Exception {
		Namespace ns = currentProgram.getGlobalNamespace();
		String[] parts = name.split("::");
		SymbolTable st = currentProgram.getSymbolTable();
		for (int i = 0; i < parts.length - 1; i++) {
			Namespace next = st.getNamespace(parts[i], ns);
			if (next == null) {
				next = st.createNameSpace(ns, parts[i], SourceType.ANALYSIS);
			}
			ns = next;
		}
		return ns;
	}

	// GridNode's type language -> a Ghidra type: u8..u64, i8..i64, f32, f64, bool, char, void,
	// Name (a /GridNode or /fb struct), with * and [N].
	private DataType resolve(String text) {
		String t = text.trim();
		int count = -1;
		if (t.endsWith("]")) {
			int open = t.lastIndexOf('[');
			count = Integer.decode(t.substring(open + 1, t.length() - 1).trim());
			t = t.substring(0, open).trim();
		}
		if (t.startsWith("const ")) {
			t = t.substring(6).trim();
		}
		int stars = 0;
		while (t.endsWith("*")) {
			stars++;
			t = t.substring(0, t.length() - 1).trim();
		}
		DataType base = primitive(t);
		if (base == null) {
			base = dtm.getDataType(CATEGORY, t);
		}
		if (base == null) {
			base = dtm.getDataType(new CategoryPath("/fb"), t);
		}
		if (base == null) {
			base = stars > 0 ? VoidDataType.dataType : Undefined8DataType.dataType;
		}
		DataType dt = base;
		for (int i = 0; i < stars; i++) {
			dt = new PointerDataType(dt, 8, dtm);
		}
		if (count > 0) {
			dt = new ArrayDataType(dt, count, dt.getLength() > 0 ? dt.getLength() : 1, dtm);
		}
		return dt;
	}

	private DataType primitive(String t) {
		switch (t) {
			case "u8": return UnsignedCharDataType.dataType;
			case "i8": return SignedByteDataType.dataType;
			case "u16": return UnsignedShortDataType.dataType;
			case "i16": return ShortDataType.dataType;
			case "u32": return UnsignedIntegerDataType.dataType;
			case "i32": return IntegerDataType.dataType;
			case "u64": return UnsignedLongLongDataType.dataType;
			case "i64": return LongLongDataType.dataType;
			case "f32": return FloatDataType.dataType;
			case "f64": return DoubleDataType.dataType;
			case "bool": return BooleanDataType.dataType;
			case "char": return CharDataType.dataType;
			case "void": return VoidDataType.dataType;
			default: return null;
		}
	}

	// ---- export ------------------------------------------------------------------------------------
	private void doExport(File file) throws Exception {
		JsonObject doc = new JsonObject();
		doc.addProperty("format", OUT_FORMAT);
		doc.addProperty("version", 1);
		doc.addProperty("program", currentProgram.getName());
		JsonArray funcs = new JsonArray();
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			if (f.getSymbol().getSource() != SourceType.USER_DEFINED) {
				continue;
			}
			JsonObject o = new JsonObject();
			o.addProperty("name", qualified(f.getSymbol()));
			o.addProperty("va", f.getEntryPoint().getOffset());
			o.addProperty("prototype", f.getPrototypeString(false, false));
			o.addProperty("signatureUserDefined", f.getSignatureSource() == SourceType.USER_DEFINED);
			o.addProperty("comment", yourComment(f.getEntryPoint(), CommentType.PLATE));
			funcs.add(o);
		}
		JsonArray labels = new JsonArray();
		SymbolIterator si = currentProgram.getSymbolTable().getSymbolIterator(true);
		while (si.hasNext() && !monitor.isCancelled()) {
			Symbol s = si.next();
			if (s.getSymbolType() != SymbolType.LABEL || s.getSource() != SourceType.USER_DEFINED || s.isExternal()) {
				continue;
			}
			// A label at a function's entry is the function's own name, already exported above.
			if (getFunctionAt(s.getAddress()) != null) {
				continue;
			}
			JsonObject o = new JsonObject();
			o.addProperty("name", qualified(s));
			o.addProperty("va", s.getAddress().getOffset());
			Data d = getDataAt(s.getAddress());
			o.addProperty("type", d != null && d.isDefined() ? d.getDataType().getDisplayName() : "");
			String c = yourComment(s.getAddress(), CommentType.PLATE);
			o.addProperty("comment", c.isEmpty() ? yourComment(s.getAddress(), CommentType.EOL) : c);
			labels.add(o);
		}
		JsonArray ss = new JsonArray();
		Iterator<DataType> dts = dtm.getAllDataTypes();
		while (dts.hasNext()) {
			DataType dt = dts.next();
			if (!(dt instanceof Structure) || !CATEGORY.equals(dt.getCategoryPath())) {
				continue;
			}
			Structure st = (Structure) dt;
			JsonArray members = new JsonArray();
			for (DataTypeComponent c : st.getDefinedComponents()) {
				if (c.getFieldName() == null) {
					continue;
				}
				JsonObject m = new JsonObject();
				m.addProperty("offset", c.getOffset());
				m.addProperty("name", c.getFieldName());
				m.addProperty("type", c.getDataType().getDisplayName());
				members.add(m);
			}
			JsonObject o = new JsonObject();
			o.addProperty("name", st.getName());
			o.add("members", members);
			ss.add(o);
		}
		doc.add("functions", funcs);
		doc.add("labels", labels);
		doc.add("structs", ss);
		Gson gson = new GsonBuilder().setPrettyPrinting().create();
		try (Writer w = new FileWriter(file)) {
			gson.toJson(doc, w);
		}
		println(String.format("GridNodeSync export: %d functions and %d labels you named, %d GridNode structs -> %s",
			funcs.size(), labels.size(), ss.size(), file));
		println("Next: python tools/gen/gridnode.py ghidra-import \"" + file + "\"");
	}

	// The part of a comment you wrote (GridNode's and relayed blocks left out).
	private String yourComment(Address a, CommentType kind) {
		CodeUnit cu = currentProgram.getListing().getCodeUnitAt(a);
		return cu == null ? "" : ownText(cu.getComment(kind));
	}

	private String qualified(Symbol s) {
		List<String> parts = new ArrayList<>();
		parts.add(s.getName());
		Namespace ns = s.getParentNamespace();
		while (ns != null && !ns.isGlobal()) {
			parts.add(0, ns.getName());
			ns = ns.getParentNamespace();
		}
		return String.join("::", parts);
	}

	// ---- helpers -------------------------------------------------------------------------------------
	private Address addr(JsonObject o) {
		return toAddr(o.get("va").getAsLong());
	}

	private static JsonArray array(JsonObject o, String key) {
		JsonElement e = o.get(key);
		return e != null && e.isJsonArray() ? e.getAsJsonArray() : new JsonArray();
	}

	private static String str(JsonObject o, String key) {
		JsonElement e = o.get(key);
		return e == null || e.isJsonNull() ? "" : e.getAsString();
	}
}

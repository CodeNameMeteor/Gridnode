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
//@category GridNode
//@author GridNode

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.Reader;
import java.io.Writer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

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
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
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
	private static final String MARK = "GridNode [";

	private DataTypeManager dtm;
	private int named, kept, structs, comments, failed;

	@Override
	protected void run() throws Exception {
		dtm = currentProgram.getDataTypeManager();
		String importChoice = "Import GridNode's annotations into Ghidra";
		String exportChoice = "Export the names I gave in Ghidra, for GridNode";
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
		JsonArray ss = doc.getAsJsonArray("structs");
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
		for (JsonElement e : doc.getAsJsonArray("functions")) {
			JsonObject f = e.getAsJsonObject();
			nameFunction(addr(f), str(f, "name"), str(f, "comment"));
		}
		for (JsonElement e : doc.getAsJsonArray("labels")) {
			JsonObject l = e.getAsJsonObject();
			nameLabel(addr(l), str(l, "name"), str(l, "type"), str(l, "comment"));
		}
		println(String.format("GridNodeSync import: %d structs, %d names set, %d kept (yours), %d comments, %d problems",
			structs, named, kept, comments, failed));
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
			setComment(a, CodeUnit.PLATE_COMMENT, comment);
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
			setComment(a, getFunctionContaining(a) != null ? CodeUnit.EOL_COMMENT : CodeUnit.PLATE_COMMENT, comment);
		}
		catch (Exception ex) {
			failed++;
			println("label " + name + " at " + a + ": " + ex.getMessage());
		}
	}

	// GridNode's comment replaces an earlier GridNode comment; any other comment is kept above it.
	private void setComment(Address a, int kind, String comment) {
		if (comment == null || comment.isEmpty()) {
			return;
		}
		CodeUnit cu = currentProgram.getListing().getCodeUnitAt(a);
		if (cu == null) {
			return;
		}
		String old = cu.getComment(kind);
		String text = comment;
		if (old != null && !old.isEmpty()) {
			int at = old.indexOf(MARK);
			String mine = at >= 0 ? old.substring(0, at).trim() : old.trim();
			text = mine.isEmpty() ? comment : mine + "\n" + comment;
		}
		if (!text.equals(old)) {
			cu.setComment(kind, text);
			comments++;
		}
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
			funcs.add(o);
		}
		JsonArray labels = new JsonArray();
		SymbolIterator si = currentProgram.getSymbolTable().getSymbolIterator(true);
		while (si.hasNext() && !monitor.isCancelled()) {
			Symbol s = si.next();
			if (s.getSymbolType() != SymbolType.LABEL || s.getSource() != SourceType.USER_DEFINED || s.isExternal()) {
				continue;
			}
			JsonObject o = new JsonObject();
			o.addProperty("name", qualified(s));
			o.addProperty("va", s.getAddress().getOffset());
			Data d = getDataAt(s.getAddress());
			o.addProperty("type", d != null && d.isDefined() ? d.getDataType().getDisplayName() : "");
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

	private static String str(JsonObject o, String key) {
		JsonElement e = o.get(key);
		return e == null || e.isJsonNull() ? "" : e.getAsString();
	}
}

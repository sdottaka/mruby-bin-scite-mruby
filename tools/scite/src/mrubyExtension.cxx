// SciTE - Scintilla based Text Editor
// mrubyExtension.cxx - mruby scripting extension based on (LuaExtension: Copyright 1998-2000 by Neil Hodgson <neilh@scintilla.org>)
// modified by s7taka@gmail.com
// The License.txt file describes the conditions under which this software may be distributed.

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include <string>
#include <vector>

#include "Scintilla.h"

#include "GUI.h"
#include "SString.h"
#include "FilePath.h"
#include "StyleWriter.h"
#include "Extender.h"
#include "mrubyExtension.h"

#include "IFaceTable.h"
#include "SciTEKeys.h"

extern "C" {
#include "mruby.h"
#include "mruby/array.h"
#include "mruby/compile.h"
#include "mruby/class.h"
#include "mruby/data.h"
#include "mruby/error.h"
#include "mruby/hash.h"
#include "mruby/irep.h"
#include "mruby/string.h"
#include "mruby/variable.h"
#include "../mrblib/mrblib_extman.c"
}

#if defined(_WIN32) && defined(_MSC_VER)

// MSVC looks deeper into the code than other compilers, sees that
// lua_error calls longjmp, and complains about unreachable code.
#pragma warning(disable: 4702)

#endif


// A note on naming conventions:
// I've gone back and forth on this a bit, trying different styles.
// It isn't easy to get something that feels consistent, considering
// that the Lua API uses lower case, underscore-separated words and
// Scintilla of course uses mixed case with no underscores.

// What I've settled on is that functions that require you to think
// about the Lua stack are likely to be mixed with Lua API functions,
// so these should using a naming convention similar to Lua itself.
// Functions that don't manipulate Lua at a low level should follow
// the normal SciTE convention.  There is some grey area of course,
// and for these I just make a judgement call

#define M_SCITE mrb_module_get(mrb, "SciTE")

static void pmo_free(mrb_state *mrb, void *ptr);
static void subprocess_free(mrb_state *mrb, void *ptr);
static mrb_data_type mrb_sc_type  = { "SciTEStylingContext", mrb_free };
static mrb_data_type mrb_pmo_type = { "SciTEPaneMatchObject", pmo_free };
static mrb_data_type mrb_po_type  = { "SciTEPane", mrb_free };
static mrb_data_type mrb_ipb_type = { "SciTEIFacePropertyBinding", mrb_free };
static mrb_data_type mrb_subprocess_type = { "SciTESubprocess", subprocess_free };

static ExtensionAPI *host = 0;
static mrb_state *mrbState = 0;
static bool mrubyDisabled = false;

static std::string startupScript;
static std::string extensionScript;

static bool tracebackEnabled = true;

static int maxBufferIndex = -1;
static int curBufferIndex = -1;

static int GetPropertyInt(const char *propName) {
	int propVal = 0;
	if (host) {
		std::string sPropVal = host->Property(propName);
		if (sPropVal.length()) {
			propVal = atoi(sPropVal.c_str());
		}
	}
	return propVal;
}

mrubyExtension::mrubyExtension() {}

mrubyExtension::~mrubyExtension() {}

mrubyExtension &mrubyExtension::Instance() {
	static mrubyExtension singleton;
	return singleton;
}

// Forward declarations
static ExtensionAPI::Pane check_pane_object(mrb_state *mrb, mrb_value self);
static mrb_value create_pane_object(mrb_state *mrb, ExtensionAPI::Pane p);
static mrb_value iface_function_helper(mrb_state *mrb, ExtensionAPI::Pane pane, const IFaceFunction &func, mrb_int argc, mrb_value *argv);
static mrb_value cf_pane_metatable_newindex(mrb_state *mrb, mrb_value self, const char *name, mrb_value val);
static void stylingcontext_init(mrb_state *mrb);
static void backtrace(mrb_state *mrb, const char *error = NULL);

inline bool IFaceTypeIsScriptable(IFaceType t, int index) {
	return t < iface_stringresult || (index==1 && t == iface_stringresult);
}

inline bool IFaceTypeIsNumeric(IFaceType t) {
	return (t > iface_void && t < iface_bool);
}

inline bool IFaceFunctionIsScriptable(const IFaceFunction &f) {
	return IFaceTypeIsScriptable(f.paramType[0], 0) && IFaceTypeIsScriptable(f.paramType[1], 1);
}

inline bool IFacePropertyIsScriptable(const IFaceProperty &p) {
	return (((p.valueType > iface_void) && (p.valueType <= iface_stringresult) && (p.valueType != iface_keymod)) &&
	        ((p.paramType < iface_colour) || (p.paramType == iface_string) ||
	                (p.paramType == iface_bool)) && (p.getter || p.setter));
}

inline void raise_error(mrb_state *mrb, const char *errMsg = NULL) {
	mrb_raisef(mrb, E_RUNTIME_ERROR, "%S", mrb_str_new_cstr(mrb, errMsg ? errMsg : ""));
}

static std::string camelize(const char *s) {
	std::string ret(s);
	if (ret.empty())
		return "";
	ret[0] = static_cast<char>(toupper(static_cast<unsigned char>(ret[0])));
	size_t j = 1, len = ret.length();
	for (size_t i = 1; i < len; ++i) {
		if (ret[i] == '_') {
			i++;
			if (i < len)
				ret[j++] = static_cast<char>(toupper(static_cast<unsigned char>(ret[i])));
		} else {
			ret[j++] = ret[i];
		}
	}
	ret.resize(j);
	return ret;
}

static const char *obj_to_cstr(mrb_state *mrb, mrb_value obj)
{
	mrb_value str = mrb_str_to_str(mrb, obj);
	const char *p = RSTRING_PTR(str);
	if (p[RSTRING_LEN(str)] == '\0')
		return p;
	return mrb_string_value_cstr(mrb, &str);
}

static mrb_value cf_scite_send(mrb_state *mrb, ExtensionAPI::Pane pane) {
	// This is reinstated as a replacement for the old <pane>:send, which was removed
	// due to safety concerns.  Is now exposed as scite.SendEditor / scite.SendOutput.
	// It is rewritten to be typesafe, checking the arguments against the metadata in
	// IFaceTable in the same way that the object interface does.

	mrb_value *argv;
	mrb_int message, argc;
	mrb_get_args(mrb, "i*", &message, &argv, &argc);

	IFaceFunction func = { "", 0, iface_void, { iface_void, iface_void } };
	for (int funcIdx = 0; funcIdx < IFaceTable::functionCount; ++funcIdx) {
		if (IFaceTable::functions[funcIdx].value == message) {
			func = IFaceTable::functions[funcIdx];
			break;
		}
	}

	if (func.value == 0) {
		for (int propIdx = 0; propIdx < IFaceTable::propertyCount; ++propIdx) {
			if (IFaceTable::properties[propIdx].getter == message) {
				func = IFaceTable::properties[propIdx].GetterFunction();
				break;
			} else if (IFaceTable::properties[propIdx].setter == message) {
				func = IFaceTable::properties[propIdx].SetterFunction();
				break;
			}
		}
	}

	if (func.value != 0) {
		if (IFaceFunctionIsScriptable(func)) {
			return iface_function_helper(mrb, pane, func, argc, argv);
		} else {
			raise_error(mrb, "Cannot call send for this function: not scriptable.");
			return mrb_nil_value();
		}
	} else {
		raise_error(mrb, "Message number does not match any published Scintilla function or property");
		return mrb_nil_value();
	}
	return mrb_nil_value();
}

static mrb_value cf_scite_send_editor(mrb_state *mrb, mrb_value /*self*/) {
	return cf_scite_send(mrb, ExtensionAPI::paneEditor);
}

static mrb_value cf_scite_send_output(mrb_state *mrb, mrb_value /*self*/) {
	return cf_scite_send(mrb, ExtensionAPI::paneOutput);
}

static mrb_value cf_scite_constname(mrb_state *mrb, mrb_value /*self*/) {
	char constName[100] = "";
	mrb_int message;
	mrb_get_args(mrb, "i", &message);
	if (IFaceTable::GetConstantName(message, constName, 100) > 0) {
		return mrb_str_new_cstr(mrb, constName);
	} else {
		raise_error(mrb, "Argument does not match any Scintilla / SciTE constant");
		return mrb_nil_value();
	}
}

static mrb_value cf_scite_open(mrb_state *mrb, mrb_value /*self*/) {
	const char *s;
	mrb_get_args(mrb, "z", &s);
	SString cmd = "open:";
	cmd += s;
	cmd.substitute("\\", "\\\\");
	host->Perform(cmd.c_str());
	return mrb_nil_value();
}

static mrb_value cf_scite_menu_command(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int cmdID;
	mrb_get_args(mrb, "i", &cmdID);
	host->DoMenuCommand(cmdID);
	return mrb_nil_value();
}

static mrb_value cf_scite_update_status_bar(mrb_state *mrb, mrb_value /*self*/) {
	mrb_bool bUpdateSlowData = false;
	mrb_get_args(mrb, "|b", &bUpdateSlowData);
	host->UpdateStatusBar(!!bUpdateSlowData);
	return mrb_nil_value();
}

static mrb_value cf_scite_strip_show(mrb_state *mrb, mrb_value /*self*/) {
	const char *s;
	mrb_get_args(mrb, "z", &s);
	host->UserStripShow(s);
	return mrb_nil_value();
}

static mrb_value cf_scite_strip_set(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int control;
	const char *value;
	mrb_get_args(mrb, "iz", &control, &value);
	host->UserStripSet(control, value);
	return mrb_nil_value();
}

static mrb_value cf_scite_strip_set_list(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int control;
	const char *value;
	mrb_get_args(mrb, "iz", &control, &value);
	host->UserStripSetList(control, value);
	return mrb_nil_value();
}

static mrb_value cf_scite_strip_value(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int control;
	mrb_get_args(mrb, "i", &control);
	const char *value = host->UserStripValue(control);
	if (value) {
		mrb_value ret = mrb_str_new_cstr(mrb, value);
		delete[]value;
		return ret;
	} else {
		return mrb_str_new_lit(mrb, "");
	}
}

struct Pane {
	ExtensionAPI::Pane pane;
};

static ExtensionAPI::Pane check_pane_object(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane pane = static_cast<Pane *>(mrb_data_get_ptr(mrb, self, &mrb_po_type))->pane;
	if ((pane == ExtensionAPI::paneEditor) && (curBufferIndex < 0))
		raise_error(mrb, "Editor pane is not accessible at this time.");
	return pane;
}

static mrb_value cf_pane_textrange(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane p = check_pane_object(mrb, self);
	mrb_int cpMin, cpMax;
	mrb_get_args(mrb, "ii", &cpMin, &cpMax);
	if (cpMax >= 0) {
		char *range = host->Range(p, cpMin, cpMax);
		if (range) {
			mrb_value ret = mrb_str_new_cstr(mrb, range);
			delete[]range;
			return ret;
		}
	} else {
		raise_error(mrb, "Invalid argument 1 for <pane>:textrange.  Positive number or zero expected.");
	}

	return mrb_nil_value();
}

static mrb_value cf_pane_insert(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane p = check_pane_object(mrb, self);
	mrb_int pos;
	const char *s;
	mrb_get_args(mrb, "iz", &pos, &s);
	host->Insert(p, pos, s);
	return mrb_nil_value();
}

static mrb_value cf_pane_remove(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane p = check_pane_object(mrb, self);
	mrb_int cpMin, cpMax;
	mrb_get_args(mrb, "ii", &cpMin, &cpMax);
	host->Remove(p, cpMin, cpMax);
	return mrb_nil_value();
}

static mrb_value cf_pane_append(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane p = check_pane_object(mrb, self);
	const char *s;
	mrb_get_args(mrb, "z", &s);
	host->Insert(p, static_cast<int>(host->Send(p, SCI_GETLENGTH, 0, 0)), s);
	return mrb_nil_value();
}

static mrb_value cf_pane_findtext(mrb_state *mrb, mrb_value self) {
	ExtensionAPI::Pane p = check_pane_object(mrb, self);
	const char *t;
	mrb_int flags = 0, cpMin = 0, cpMax = 0;
	int nArgs = mrb_get_args(mrb, "z|iii", &t, &flags, &cpMin, &cpMax);
	Sci_TextToFind ft = { { 0, 0 }, 0, { 0, 0 } };
	ft.lpstrText = const_cast<char *>(t);
	if (nArgs > 2) {
		ft.chrg.cpMin = cpMin;
	}
	if (nArgs > 3) {
		ft.chrg.cpMax = cpMax;
	} else {
		ft.chrg.cpMax = static_cast<long>(host->Send(p, SCI_GETLENGTH, 0, 0));
	}
	sptr_t result = host->Send(p, SCI_FINDTEXT, static_cast<uptr_t>(flags), reinterpret_cast<sptr_t>(&ft));
	if (result >= 0) {
		mrb_value vals[] = { mrb_fixnum_value(ft.chrgText.cpMin), mrb_fixnum_value(ft.chrgText.cpMax) };
		return mrb_ary_new_from_values(mrb, 2, vals);
	} else {
		return mrb_nil_value();
	}
}

// Pane match generator.  This was prototyped in about 30 lines of Lua.
// I hope the C++ version is more robust at least, e.g. prevents infinite
// loops and is more tamper-resistant.

struct PaneMatchObject {
	ExtensionAPI::Pane pane;
	int startPos;
	int endPos;
	int flags; // this is really part of the state, but is kept here for convenience
	int endPosOrig; // has to do with preventing infinite loop on a 0-length match
	char *text;
};

static void pmo_free(mrb_state *mrb, void *ptr) {
	if (ptr) {
		PaneMatchObject *pmo = static_cast<PaneMatchObject *>(ptr);
		if (pmo->text)
			mrb_free(mrb, pmo->text);
		pmo->text = NULL;
		mrb_free(mrb, ptr);
	}
}

static mrb_value cf_match_replace(mrb_state *mrb, PaneMatchObject *pmo, const char *replacement, mrb_int len) {
	if ((pmo->startPos < 0) || (pmo->endPos < pmo->startPos) || (pmo->endPos < 0)) {
		raise_error(mrb, "Blocked attempt to use invalidated pane match object.");
		return mrb_nil_value();
	}

	// If an option were added to process \d back-references, it would just
	// be an optional boolean argument, i.e. m:replace([[\1]], true), and
	// this would just change SCI_REPLACETARGET to SCI_REPLACETARGETRE.
	// The problem is, even if SCFIND_REGEXP was used, it's hard to know
	// whether the back references are still valid.  So for now this is
	// left out.

	host->Send(pmo->pane, SCI_SETTARGETSTART, pmo->startPos, 0);
	host->Send(pmo->pane, SCI_SETTARGETEND, pmo->endPos, 0);
	host->Send(pmo->pane, SCI_REPLACETARGET, len, reinterpret_cast<sptr_t>(replacement));
	pmo->endPos = static_cast<int>(host->Send(pmo->pane, SCI_GETTARGETEND, 0, 0));
	return mrb_nil_value();
}

static mrb_value cf_match_metatable_index(mrb_state *mrb, mrb_value self) {
	PaneMatchObject *pmo = reinterpret_cast<PaneMatchObject *>(mrb_data_get_ptr(mrb, self, &mrb_pmo_type));
	if (!pmo) {
		raise_error(mrb, "Internal error: pane match object is missing.");
		return mrb_nil_value();
	} else if ((pmo->startPos < 0) || (pmo->endPos < pmo->startPos) || (pmo->endPos < 0)) {
		raise_error(mrb, "Blocked attempt to use invalidated pane match object.");
		return mrb_nil_value();
	}

	char *replaceText = NULL;
	mrb_int len;
	mrb_sym mid;
	mrb_get_args(mrb, "n|s", &mid, &replaceText, &len);

	if (mid == mrb_intern_lit(mrb, "pos")) {
		return mrb_fixnum_value(pmo->startPos);
	} else if (mid == mrb_intern_lit(mrb, "len")) {
		return mrb_fixnum_value(pmo->endPos - pmo->startPos);
	} else if (mid == mrb_intern_lit(mrb, "text")) {
		// If the document is changed while in the match loop, this will be broken.
		// Exception: if the changes are made exclusively through match:replace,
		// everything will be fine.
		char *range = host->Range(pmo->pane, pmo->startPos, pmo->endPos);
		if (range) {
			mrb_value ret = mrb_str_new_cstr(mrb, range);
			delete[]range;
			return ret;
		} else {
			return mrb_nil_value();
		}
	} else if (mid == mrb_intern_lit(mrb, "replace")) {
		return cf_match_replace(mrb, pmo, replaceText, len);
	}

	raise_error(mrb, "Invalid property / method name for pane match object.");
	return mrb_nil_value();
}

static mrb_value cf_match_metatable_tostring(mrb_state *mrb, mrb_value self) {
	PaneMatchObject *pmo = reinterpret_cast<PaneMatchObject *>(mrb_data_get_ptr(mrb, self, &mrb_pmo_type));
	if (!pmo) {
		raise_error(mrb, "Internal error: pane match object is missing.");
		return mrb_nil_value();
	} else if ((pmo->startPos < 0) || (pmo->endPos < pmo->startPos) || (pmo->endPos < 0)) {
		return mrb_str_new_lit(mrb, "match(invalidated)");
	} else {
		char buf[256];
		sprintf(buf, "match{pos=%d,len=%d}", pmo->startPos, pmo->endPos - pmo->startPos);
		return mrb_str_new_cstr(mrb, buf);
	}
}

static mrb_value cf_pane_match(mrb_state *mrb, mrb_value self) {
	mrb_int flags = 0, startPos = 0;
	char *text;
	mrb_int len;
	int nargs = mrb_get_args(mrb, "s|ii", &text, &len, &flags, &startPos);

	ExtensionAPI::Pane p = check_pane_object(mrb, self);

	// I'm putting some of the state in the match userdata for more convenient
	// access.  But, the search string is going in state because that part is
	// more convenient to leave in Lua form.

	PaneMatchObject *pmo = static_cast<PaneMatchObject *>(mrb_malloc(mrb, sizeof(PaneMatchObject)));
	if (pmo) {
		pmo->pane = p;
		pmo->startPos = -1;
		pmo->endPos = pmo->endPosOrig = 0;
		pmo->flags = 0;
		pmo->text = static_cast<char *>(mrb_malloc(mrb, len + 1));
		strcpy(pmo->text, text);
		if (nargs >= 2) {
			pmo->flags = flags;
			if (nargs >= 3) {
				pmo->endPos = pmo->endPosOrig = startPos;
				if (pmo->endPos < 0) {
					raise_error(mrb, "Invalid argument 3 for <pane>:match.  Positive number or zero expected.");
					return mrb_nil_value();
				}
			}
		}
		RClass *pane_match_object_class = mrb_class_get_under(mrb, M_SCITE, "PaneMatchObject");
		return mrb_obj_value(mrb_data_object_alloc(mrb, pane_match_object_class, pmo, &mrb_pmo_type));
	} else {
		raise_error(mrb, "Internal error: could not create match object.");
		return mrb_nil_value();
	}
}

static mrb_value cf_pane_match_generator(mrb_state *mrb, mrb_value self) {
	PaneMatchObject *pmo = reinterpret_cast<PaneMatchObject *>(mrb_data_get_ptr(mrb, self, &mrb_pmo_type));
	const char *text = pmo->text;

	if (!(text)) {
		raise_error(mrb, "Internal error: invalid state for <pane>:match generator.");
		return mrb_nil_value();
	} else if (!pmo) {
		raise_error(mrb, "Internal error: invalid match object initializer for <pane>:match generator");
		return mrb_nil_value();
	}

	if ((pmo->endPos < 0) || (pmo->endPos < pmo->startPos)) {
		raise_error(mrb, "Blocked attempt to use invalidated pane match object.");
		return mrb_nil_value();
	}

	int searchPos = pmo->endPos;
	if ((pmo->startPos == pmo->endPosOrig) && (pmo->endPos == pmo->endPosOrig)) {
		// prevent infinite loop on zero-length match by stepping forward
		searchPos++;
	}

	Sci_TextToFind ft = { {0,0}, 0, {0,0} };
	ft.chrg.cpMin = searchPos;
	ft.chrg.cpMax = static_cast<long>(host->Send(pmo->pane, SCI_GETLENGTH, 0, 0));
	ft.lpstrText = const_cast<char *>(text);

	if (ft.chrg.cpMax > ft.chrg.cpMin) {
		sptr_t result = host->Send(pmo->pane, SCI_FINDTEXT, static_cast<uptr_t>(pmo->flags), reinterpret_cast<sptr_t>(&ft));
		if (result >= 0) {
			pmo->startPos = static_cast<int>(ft.chrgText.cpMin);
			pmo->endPos = pmo->endPosOrig = static_cast<int>(ft.chrgText.cpMax);
			return mrb_str_new_cstr(mrb, text);
		}
	}

	// One match object is used throughout the entire iteration.
	// This means it's bad to try to save the match object for later
	// reference.
	pmo->startPos = pmo->endPos = pmo->endPosOrig = -1;
	return mrb_nil_value();
}

static mrb_value cf_pane_match_each(mrb_state *mrb, mrb_value self) {
	mrb_value block;
	mrb_get_args(mrb, "&", &block);
	if (mrb_nil_p(block))
		return mrb_funcall(mrb, self, "to_enum", 0);

	mrb_value ret;
	int ai = mrb_gc_arena_save(mrb);
	for (;;) {
		ret = cf_pane_match_generator(mrb, self);
		mrb_gc_arena_restore(mrb, ai);
		if (mrb_nil_p(ret))
			break;
		mrb_yield(mrb, block, self);
	}
	return mrb_nil_value();
}

static mrb_value cf_props_metatable_index(mrb_state *mrb, mrb_value /*self*/) {
	char *key;
	mrb_get_args(mrb, "z", &key);
	std::string value = host->Property(key);
	return mrb_str_new_cstr(mrb, value.c_str());
}

static mrb_value cf_props_metatable_newindex(mrb_state *mrb, mrb_value /*self*/) {
	const char *key;
	mrb_value oval;
	mrb_get_args(mrb, "zo", &key, &oval);
	if (!mrb_nil_p(oval)) {
		const char *val = obj_to_cstr(mrb, oval);
		host->SetProperty(key, val);
	} else {
		host->UnsetProperty(key);
	}
	return mrb_nil_value();
}

/*
static mrb_value cf_os_execute(mrb_state *mrb, mrb_value self) {
	// The SciTE version of os.execute would pipe its stdout and stderr
	// to the output pane.  This can be implemented in terms of popen
	// on GTK and in terms of CreateProcess on Windows.  Either way,
	// stdin should be null, and the Lua script should wait for the
	// subprocess to finish before continuing.  (What if it takes
	// a very long time?  Timeout?)

	raise_error(mrb, "Not implemented.");
	return mrb_nil_value();
}
*/

static mrb_value cf_global_print_str(mrb_state *mrb, mrb_value /*self*/) {
	mrb_value argv;
	mrb_get_args(mrb, "o", &argv);
	if (mrb_string_p(argv))
		host->Trace(RSTRING_PTR(argv));
	return argv;
}

static mrb_value cf_global_print(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int nargs;
	mrb_value *argv;
	mrb_get_args(mrb, "*", &argv, &nargs);
	for (mrb_int i = 0; i < nargs; ++i) {
		host->Trace(obj_to_cstr(mrb, argv[i]));
	}
	return mrb_nil_value();
}

static mrb_value cf_global_puts(mrb_state *mrb, mrb_value /*self*/) {
	mrb_int nargs;
	mrb_value *argv;
	mrb_get_args(mrb, "*", &argv, &nargs);
	for (mrb_int i = 0; i < nargs; ++i) {
		SString msg = obj_to_cstr(mrb, argv[i]);
		if (msg.length() > 0 && msg[msg.length() - 1] != '\n')
			msg += "\n";
		host->Trace(msg.c_str());
	}
	if (nargs == 0)
		host->Trace("\n");
	return mrb_nil_value();
}

static mrb_value cf_global_trace(mrb_state *mrb, mrb_value /*self*/) {
	const char *s;
	mrb_get_args(mrb, "z", &s);
	host->Trace(s);
	return mrb_nil_value();
}

static mrb_value cf_global_dostring(mrb_state *mrb, mrb_value /*self*/) {
	char *s;
	int len;
	mrb_get_args(mrb, "s", &s, &len);
	return mrb_load_nstring(mrb, s, len);
}

static bool call_function(mrb_state *mrb, mrb_sym mid, int nargs, mrb_value *argv, bool ignoreFunctionReturnValue = false) {
	if (mrb) {
		mrb_value ret = mrb_funcall_argv(mrb, mrb_obj_value(mrb->top_self), mid, nargs, argv);
		if (mrb->exc) {
			SString msg = ">mruby: an error occurred in the function ";
			msg += mrb_sym2name(mrb, mid);
			msg += "\n";
			msg += obj_to_cstr(mrb, mrb_inspect(mrb, mrb_obj_value(mrb->exc)));
			msg += "\n";
			host->Trace(msg.c_str());
			mrb->exc = NULL;
		}
		if (ignoreFunctionReturnValue)
			return true;
		else if (mrb_nil_p(ret))
			return false;
		else
			return mrb_bool(ret);
	}
	return false;
	/* FIXME:
	bool handled = false;
	if (mrb) {
		int traceback = 0;
		if (tracebackEnabled) {
			lua_getglobal(mrb, "debug");
			lua_getfield(mrb, -1, "traceback");
			lua_remove(mrb, -2);
			if (lua_isfunction(mrb, -1)) {
				traceback = lua_gettop(mrb) - nargs - 1;
				lua_insert(mrb, traceback);
			} else {
				lua_pop(mrb, 1);
			}
		}

		int result = lua_pcall(mrb, nargs, ignoreFunctionReturnValue ? 0 : 1, traceback);

		if (traceback) {
			lua_remove(mrb, traceback);
		}

		if (0 == result) {
			if (ignoreFunctionReturnValue) {
				handled = true;
			} else {
				handled = (0 != lua_toboolean(mrb, -1));
				lua_pop(mrb, 1);
			}
		} else if (result == LUA_ERRRUN) {
			lua_getglobal(mrb, "print");
			lua_insert(mrb, -2); // use pushed error message
			lua_pcall(mrb, 1, 0, 0);
		} else {
			lua_pop(mrb, 1);
			if (result == LUA_ERRMEM) {
				host->Trace("> Lua: memory allocation error\n");
			} else if (result == LUA_ERRERR) {
				host->Trace("> Lua: an error occurred, but cannot be reported due to failure in _TRACEBACK\n");
			} else {
				host->Trace("> Lua: unexpected error\n");
			}
		}
	}
	return handled;
	*/
	return false;
}

static bool CallNamedFunction(const char *name) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_cstr(mrbState, name);
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			handled = call_function(mrbState, mid, 0, NULL);
		}
	}
	return handled;
}

static bool CallNamedFunction(const char *name, const char *arg) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_cstr(mrbState, name);
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			mrb_value argv[] = { mrb_str_new_cstr(mrbState, arg) };
			handled = call_function(mrbState, mid, 1, argv);
		}
	}
	return handled;
}

static bool CallNamedFunction(const char *name, int numberArg, const char *stringArg) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_cstr(mrbState, name);
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			mrb_value argv[] = { mrb_fixnum_value(numberArg), mrb_str_new_cstr(mrbState, stringArg) };
			handled = call_function(mrbState, mid, 2, argv);
		}
	}
	return handled;
}

static bool CallNamedFunction(const char *name, int numberArg, int numberArg2) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_cstr(mrbState, name);
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			mrb_value argv[] = { mrb_fixnum_value(numberArg), mrb_fixnum_value(numberArg2) };
			handled = call_function(mrbState, mid, 2, argv);
		}
	}
	return handled;
}

static mrb_value iface_function_helper(mrb_state *mrb, ExtensionAPI::Pane p, const IFaceFunction &func, mrb_int argc, mrb_value *argv) {
	int arg = 0;

	sptr_t params[2] = { 0, 0 };

	char *stringResult = 0;
	bool needStringResult = false;

	int loopParamCount = 2;

	if (func.paramType[0] == iface_length && func.paramType[1] == iface_string) {
		mrb_value str = (arg < argc) ? mrb_str_to_str(mrb, argv[arg]) : mrb_str_new_cstr(mrb, "");
		params[0] = RSTRING_LEN(str);
		params[1] = reinterpret_cast<sptr_t>(params[0] ? RSTRING_PTR(str) : "");
		loopParamCount = 0;
	} else if ((func.paramType[1] == iface_stringresult) || (func.returnType == iface_stringresult)) {
		needStringResult = true;
		// The buffer will be allocated later, so it won't leak if Lua does
		// a longjmp in response to a bad arg.
		if (func.paramType[0] == iface_length) {
			loopParamCount = 0;
		} else {
			loopParamCount = 1;
		}
	}

	for (int i=0; i<loopParamCount; ++i) {
		if (func.paramType[i] == iface_string) {
			const char *s = (arg < argc) ? obj_to_cstr(mrb, argv[arg++]) : NULL;
			params[i] = reinterpret_cast<sptr_t>(s ? s : "");
		} else if (func.paramType[i] == iface_keymod) {
			int keycode = (arg < argc) ? (static_cast<int>(mrb_fixnum(mrb_to_int(mrb, argv[arg++])) & 0xFFFF)) : 0;
			int modifiers = (arg < argc) ? (static_cast<int>(mrb_fixnum(mrb_to_int(mrb, argv[arg++])) & (SCMOD_SHIFT | SCMOD_CTRL | SCMOD_ALT))) : 0;
			params[i] = keycode | (modifiers << 16);
		} else if (func.paramType[i] == iface_bool) {
			params[i] = (arg < argc) ? mrb_bool(argv[arg++]) : false;
		} else if (IFaceTypeIsNumeric(func.paramType[i])) {
			params[i] = (arg < argc) ? static_cast<long>(mrb_fixnum(mrb_to_int(mrb, argv[arg++]))) : 0;
		} else if (func.paramType[i] == iface_void && i > 0) {
			params[i] = params[i - 1];
		}
	}
	sptr_t stringResultLen = 0;
	if (needStringResult) {
		stringResultLen = host->Send(p, func.value, params[0], 0);
		if (stringResultLen >= 0) {
			// not all string result methods are guaranteed to add a null terminator
			stringResult = new char[stringResultLen + 1];
			stringResult[stringResultLen] = '\0';
			params[1] = reinterpret_cast<sptr_t>(stringResult);
		} else {
			// Is this an error?  Are there any cases where it's not an error,
			// and where the right thing to do is just return a blank string?
			return mrb_nil_value();
		}
		if (func.paramType[0] == iface_length) {
			params[0] = stringResultLen;
		}
	}

	// Now figure out what to do with the param types and return type.
	// - stringresult gets inserted at the start of return tuple.
	// - numeric return type gets returned to lua as a number (following the stringresult)
	// - other return types e.g. void get dropped.

	sptr_t result = host->Send(p, func.value, params[0], params[1]);

	if (stringResult) {
		if (stringResultLen > 0 && stringResult[stringResultLen - 1] == 0)
			--stringResultLen;
		mrb_value ret = mrb_str_new(mrb, stringResult, stringResultLen);
		delete[] stringResult;
		return ret;
	}

	if (func.returnType == iface_bool) {
		return mrb_bool_value(!!result);
	} else if (IFaceTypeIsNumeric(func.returnType)) {
		return mrb_fixnum_value(result);
	}

	return mrb_nil_value();
}

struct IFacePropertyBinding {
	ExtensionAPI::Pane pane;
	const IFaceProperty *prop;
};

static mrb_value cf_ifaceprop_metatable_index(mrb_state *mrb, mrb_value self) {
	// if there is a getter, __index calls it
	// otherwise, __index raises "property 'name' is write-only".
	IFacePropertyBinding *ipb = reinterpret_cast<IFacePropertyBinding *>(mrb_data_get_ptr(mrb, self, &mrb_ipb_type));
	if (!(ipb && IFacePropertyIsScriptable(*(ipb->prop)))) {
		raise_error(mrb, "Internal error: property binding is improperly set up");
		return mrb_nil_value();
	}
	if (ipb->prop->getter == 0) {
		raise_error(mrb, "Attempt to read a write-only indexed property");
		return mrb_nil_value();
	}
	IFaceFunction func = ipb->prop->GetterFunction();

	mrb_value *argv;
	mrb_int argc;
	mrb_get_args(mrb, "*", &argv, &argc);

	// rewrite the stack to match what the function expects.  put pane at index 1; param is already at index 2.
	return iface_function_helper(mrb, ipb->pane, func, argc, argv);
}

static mrb_value cf_ifaceprop_metatable_newindex(mrb_state *mrb, mrb_value self) {
	IFacePropertyBinding *ipb = reinterpret_cast<IFacePropertyBinding *>(mrb_data_get_ptr(mrb, self, &mrb_ipb_type));
	if (!(ipb && IFacePropertyIsScriptable(*(ipb->prop)))) {
		raise_error(mrb, "Internal error: property binding is improperly set up");
		return mrb_nil_value();
	}
	if (ipb->prop->setter == 0) {
		raise_error(mrb, "Attempt to write a read-only indexed property");
		return mrb_nil_value();
	}
	IFaceFunction func = ipb->prop->SetterFunction();

	mrb_value *argv;
	mrb_int argc;
	mrb_get_args(mrb, "*", &argv, &argc);

	// rewrite the stack to match what the function expects.
	// pane at index 1; param at index 2, value at index 3
	return iface_function_helper(mrb, ipb->pane, func, argc, argv);
}

static int push_iface_function(mrb_state *mrb, mrb_value self, const char *name, mrb_int argc, mrb_value *argv, mrb_value *ret) {
	int i = IFaceTable::FindFunction(camelize(name).c_str());
	if (i >= 0) {
		if (IFaceFunctionIsScriptable(IFaceTable::functions[i])) {
			*ret = iface_function_helper(mrb, check_pane_object(mrb, self), IFaceTable::functions[i], argc, argv);
			return 1;
		}
	}
	return -1; // signal to try next pane index handler
}

static int push_iface_propval(mrb_state *mrb, mrb_value self, const char *name, mrb_int argc, mrb_value *argv, mrb_value *ret) {
	// this function doesn't raise errors, but returns 0 if the function is not handled.

	int propidx = IFaceTable::FindProperty(camelize(name).c_str());
	if (propidx >= 0) {
		const IFaceProperty &prop = IFaceTable::properties[propidx];
		if (!IFacePropertyIsScriptable(prop)) {
			raise_error(mrb, "Error: iface property is not scriptable.");
			return -1;
		}

		if (prop.paramType == iface_void) {
			if (prop.getter) {
				*ret = iface_function_helper(mrb, check_pane_object(mrb, self), prop.GetterFunction(), argc, argv);
				return 1;
			}
		} else if (prop.paramType == iface_bool) {
			// The bool getter is untested since there are none in the iface.
			// However, the following is suggested as a reference protocol.
			ExtensionAPI::Pane p = check_pane_object(mrb, self);

			if (prop.getter) {
				if (host->Send(p, prop.getter, 1, 0)) {
					*ret = mrb_nil_value();
					return 1;
				} else {
					*ret = iface_function_helper(mrb, check_pane_object(mrb, self), prop.GetterFunction(), argc, argv);
					return 1;
				}
			}
		} else {
			// Indexed property.  These return an object with the following behavior:
			// if there is a getter, __index calls it
			// otherwise, __index raises "property 'name' is write-only".
			// if there is a setter, __newindex calls it
			// otherwise, __newindex raises "property 'name' is read-only"

			IFacePropertyBinding *ipb = static_cast<IFacePropertyBinding *>(mrb_malloc(mrb, sizeof(IFacePropertyBinding)));
			if (ipb) {
				ipb->pane = check_pane_object(mrb, self);
				ipb->prop = &prop;
				RClass *ifaceprop_class = mrb_class_get_under(mrb, M_SCITE, "IFacePropertyBinding");
				*ret = mrb_obj_value(mrb_data_object_alloc(mrb, ifaceprop_class, ipb, &mrb_ipb_type));
				return 1;
			} else {
				raise_error(mrb, "Internal error: failed to allocate userdata for indexed property");
				return -1;
			}
		}
	}

	return -1; // signal to try next pane index handler
}

static mrb_value cf_pane_metatable_index(mrb_state *mrb, mrb_value self) {
	mrb_value oname;
	mrb_int argc;
	mrb_value *argv;
	mrb_get_args(mrb, "o|*", &oname, &argv, &argc);
	mrb_value str = mrb_str_to_str(mrb, oname);
	const char *name = RSTRING_PTR(str);
	size_t len = RSTRING_LEN(str);

	if (len > 0 && name[len - 1] == '=')
		return cf_pane_metatable_newindex(mrb, self, std::string(name, len - 1).c_str(), argv[0]);

	// these return the number of values pushed (possibly 0), or -1 if no match
	mrb_value ret;
	int results = push_iface_function(mrb, self, name, argc, argv, &ret);
	if (results < 0)
		results = push_iface_propval(mrb, self, name, argc, argv, &ret);

	if (results >= 0) {
		return ret;
	}

	raise_error(mrb, "Pane function / readable property / indexed writable property name expected");
	return mrb_nil_value();
}

static mrb_value cf_pane_metatable_newindex(mrb_state *mrb, mrb_value self, const char *name, mrb_value val) {
	int propidx = IFaceTable::FindProperty(camelize(name).c_str());
	if (propidx >= 0) {
		const IFaceProperty &prop = IFaceTable::properties[propidx];
		if (IFacePropertyIsScriptable(prop)) {
			if (prop.setter) {
				if (prop.paramType == iface_void) {
					return iface_function_helper(mrb, check_pane_object(mrb, self), prop.SetterFunction(), 1, &val);
				} else {
					raise_error(mrb, "Error - (pane object) cannot assign directly to indexed property");
				}
			} else {
				raise_error(mrb, "Error - (pane object) cannot assign to a read-only property");
			}
		}
	}

	raise_error(mrb, "Error - (pane object) expected the name of a writable property");
	return mrb_nil_value();
}

mrb_value create_pane_object(mrb_state *mrb, ExtensionAPI::Pane p) {
	RClass *pane_class = mrb_class_get_under(mrb, M_SCITE, "Pane");
	Pane *po = static_cast<Pane *>(mrb_malloc(mrb, sizeof(Pane)));
	po->pane = p;
	return mrb_obj_value(mrb_data_object_alloc(mrb, pane_class, po, &mrb_po_type));
}

static mrb_value cf_global_metatable_index(mrb_state *mrb, mrb_value self) {
	mrb_sym sym;
	mrb_get_args(mrb, "n", &sym);
	const char *name = mrb_sym2name(mrb, sym);
	
	if ((name[0] < 'A') || (name[0] > 'Z') || ((name[1] >= 'a') && (name[1] <= 'z'))) {
		// short circuit; iface constants are always upper-case and start with a letter
		return mrb_nil_value();
	}

	int i = IFaceTable::FindConstant(name);
	if (i >= 0) {
		return mrb_fixnum_value(IFaceTable::constants[i].value);
	} else {
		i = IFaceTable::FindFunctionByConstantName(name);
		if (i >= 0) {
			return mrb_fixnum_value(IFaceTable::functions[i].value);

			// FindFunctionByConstantName is slow, so cache the result into the
			// global table.  My tests show this gives an order of magnitude
			// improvement.
			/* FIXME: */
		}
	}

	if (mrb_class_real(mrb_class_ptr(self)) != mrb->object_class) {
		mrb_name_error(mrb, sym, "uninitialized constant %S::%S", self, mrb_sym2str(mrb, sym));
	} else {
	    mrb_name_error(mrb, sym, "uninitialized constant %S", mrb_sym2str(mrb, sym));
	}
	return mrb_nil_value(); // global namespace access should not raise errors
}

#ifdef _WIN32

#include <windows.h>
#include <process.h>

static WNDPROC org_wndproc;
static HWND hwndSciTE;

#else

#include <glib.h>
#include <gdk/gdk.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux)
#include <pty.h>
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#endif

static void subprocess_reapchild(GPid pid, gint status, gpointer user_data);

#endif

struct Subprocess {
#ifdef _WIN32
	PROCESS_INFORMATION pi;
	HANDLE hPipeRead;
#else
	GPid pid;
	guint inputHandle;
	int fd_pty_master;
	GIOChannel *inputChannel;
#endif
	int exitcode;
	mrb_state *mrb;
	mrb_value self;
	bool exited;
};

#ifdef _WIN32
static unsigned __stdcall
subprocess_read_output_thread(void *p)
{
	Subprocess *psp = static_cast<Subprocess *>(p);
	for (;;) {
		DWORD dwAvail;
		BOOL bSucceeded = ::PeekNamedPipe(psp->hPipeRead, NULL, 0, NULL, &dwAvail, NULL);
		if (!bSucceeded) {
			::SendMessage(hwndSciTE, WM_USER + 2000, reinterpret_cast<WPARAM>(psp), 1);
			break;
		}
		if (dwAvail > 0) {
			::SendMessage(hwndSciTE, WM_USER + 2000, reinterpret_cast<WPARAM>(psp), 0);
		} else {
			Sleep(1);
		}
	}
	return 0;
}

#endif

static void
subprocess_close_pipes(Subprocess *psp)
{
#ifdef _WIN32
	if (psp->hPipeRead) {
		::CloseHandle(psp->hPipeRead);
	}
	psp->hPipeRead = NULL;
#else
	if (psp->inputChannel) {
		g_source_remove(psp->inputHandle);
		g_io_channel_unref(psp->inputChannel);
	}
	if (psp->fd_pty_master != -1) {
		close(psp->fd_pty_master);
	}
	psp->inputChannel = NULL;
	psp->inputHandle = 0;
	psp->fd_pty_master = -1;
#endif
}

static void
subprocess_close_process_handle(Subprocess *psp)
{
#ifdef _WIN32
	if (psp->pi.hProcess) {
		::CloseHandle(psp->pi.hProcess);
		::CloseHandle(psp->pi.hThread);
	}
	psp->pi.hProcess = NULL;
	psp->pi.hThread = NULL;
#else
	if (!psp->exited)
		g_spawn_close_pid(psp->pid);
#endif
}

static bool
subprocess_pipe_closed(Subprocess *psp)
{
#ifdef _WIN32
	return (psp->hPipeRead) ? false : true;
#else
	return (psp->fd_pty_master == -1);
#endif
}

static void
subprocess_update_status(Subprocess *psp)
{
	if (psp->exited)
		return;
#ifdef _WIN32
	if (::WaitForSingleObject(psp->pi.hProcess, 0) == WAIT_TIMEOUT)
		return;
	DWORD dwExitCode;
	::GetExitCodeProcess(psp->pi.hProcess, &dwExitCode);
	psp->exitcode = dwExitCode;
#else
	if (waitpid(psp->pid, &psp->exitcode, WNOHANG) == 0)
		return;
	subprocess_reapchild(psp->pid, WEXITSTATUS(psp->exitcode), psp);
#endif
	psp->exited = true;
	subprocess_close_process_handle(psp);
}

#ifdef _WIN32

static LRESULT CALLBACK
subprocess_subclass_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_USER + 2000) {
		Subprocess *psp = reinterpret_cast<Subprocess *>(wParam);
		mrb_state *mrb = psp->mrb;
		mrb_value handler = mrb_iv_get(mrb, psp->self, mrb_intern_lit(mrb, "handler"));
		mrb_value argv[2] = { psp->self, mrb_fixnum_value(static_cast<int>(lParam)) };
		if (lParam == 1) {
			if (psp->pi.hProcess)
				::WaitForSingleObject(psp->pi.hProcess, INFINITE);
			subprocess_update_status(psp);
			if (!mrb_nil_p(handler)) {
				mrb_yield_argv(mrb, handler, 2, argv);
			}
		} else {
			if (psp->hPipeRead) {
				DWORD dwAvail;
				BOOL bSucceeded = ::PeekNamedPipe(psp->hPipeRead, NULL, 0, NULL, &dwAvail, NULL);
				if (bSucceeded) {
					if (dwAvail > 0 && !mrb_nil_p(handler)) {
						mrb_yield_argv(mrb, handler, 2, argv);
					}
				} else {
					subprocess_close_pipes(psp);
				}
			}
		}
		if (mrb->exc) {
			backtrace(mrb, ">mruby: an error occured in SciTE::Subprocess event handler\n");
			mrb->exc = NULL;
		}
	}
	return CallWindowProc(org_wndproc, hwnd, msg, wParam, lParam);
}

#else

static void
subprocess_reapchild(GPid pid, gint status, gpointer user_data)
{
	Subprocess *psp = reinterpret_cast<Subprocess *>(user_data);

	subprocess_close_process_handle(psp);

	psp->exitcode = status;
	psp->exited = true;

	mrb_state *mrb = psp->mrb;
	mrb_value handler = mrb_iv_get(mrb, psp->self, mrb_intern_lit(mrb, "handler"));
	if (!mrb_nil_p(handler)) {
		mrb_value argv[2] = { psp->self, mrb_fixnum_value(1) };
		mrb_yield_argv(mrb, handler, 2, argv);
		if (mrb->exc) {
			backtrace(mrb, ">mruby: an error occured in SciTE::Subprocess event handler\n");
			mrb->exc = NULL;
		}
	}
}

static gboolean
subprocess_iosignal(GIOChannel *, GIOCondition, Subprocess *psp) 
{
#ifndef GDK_VERSION_3_6
	gdk_threads_enter();
#endif

	mrb_state *mrb = psp->mrb;
	mrb_value handler = mrb_iv_get(mrb, psp->self, mrb_intern_lit(mrb, "handler"));
	if (!mrb_nil_p(handler)) {
		mrb_value argv[2] = { psp->self, mrb_fixnum_value(0) };
		mrb_yield_argv(mrb, handler, 2, argv);
		if (mrb->exc) {
			backtrace(mrb, ">mruby: an error occured in SciTE::Subprocess event handler\n");
			mrb->exc = NULL;
		}
	}

#ifndef GDK_VERSION_3_6
	gdk_threads_leave();
#endif
	return TRUE;
}

#endif

static bool
subprocess_spawn(mrb_state *mrb, Subprocess *psp, mrb_int argc, mrb_value *argv)
{
#ifdef _WIN32
	HWND hwndConsole = ::GetConsoleWindow();
	if (!hwndConsole)
		::AllocConsole();
	::ShowWindow(::GetConsoleWindow(), SW_HIDE);
	::SetForegroundWindow(hwndSciTE);

	SECURITY_DESCRIPTOR sd;
	::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	::SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

	SECURITY_ATTRIBUTES sa = { 0 };
	sa.lpSecurityDescriptor = &sd;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE hPipeWrite = NULL;
	::CreatePipe(&psp->hPipeRead, &hPipeWrite, &sa, 0);

	::SetHandleInformation(psp->hPipeRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si = { 0 };
	si.cb = sizeof(STARTUPINFO);
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = hPipeWrite;
	si.hStdError = hPipeWrite;

	std::string path;
	for (mrb_int i = 0; i < argc; ++i) {
		path += mrb_string_value_cstr(mrb, &argv[i]);
		if (i < argc - 1)
			path += " ";
	}

	BOOL running = ::CreateProcessW(
		NULL, const_cast<wchar_t *>(GUI::StringFromUTF8(path.c_str()).c_str()),
		NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP,
		NULL, NULL, &si, &psp->pi);

	::CloseHandle(hPipeWrite);

	if (!running) {
		::CloseHandle(psp->pi.hProcess);
		::CloseHandle(psp->pi.hThread);
		::CloseHandle(psp->hPipeRead);
		return false;
	}
	return true;
#else
	std::vector<const char *> cargv(argc + 1);
	for (mrb_int i = 0; i < argc; ++i) {
		cargv[i] = mrb_string_value_cstr(mrb, &argv[i]);
	}
	cargv[argc] = NULL;

	psp->pid = forkpty(&psp->fd_pty_master, NULL, NULL, NULL);
	if (psp->pid == 0) {
		execvp(cargv[0], const_cast<char * const *>(&cargv[0]));
		_exit(EXIT_FAILURE);
	}

	mrb_value handler = mrb_iv_get(mrb, psp->self, mrb_intern_lit(mrb, "handler"));
	if (!mrb_nil_p(handler)) {
		g_child_watch_add(psp->pid, subprocess_reapchild, psp);
		psp->inputChannel = g_io_channel_unix_new(psp->fd_pty_master);
		g_io_channel_set_encoding(psp->inputChannel, NULL, NULL);
		g_io_channel_set_buffered(psp->inputChannel, FALSE);
		g_io_channel_set_flags(psp->inputChannel, 
		    static_cast<GIOFlags>(g_io_channel_get_flags(psp->inputChannel) | G_IO_FLAG_NONBLOCK),
		    NULL);
		psp->inputHandle = g_io_add_watch(psp->inputChannel, G_IO_IN, (GIOFunc)subprocess_iosignal, psp);
	}
	return true;
#endif
}

static void
subprocess_free(mrb_state *mrb, void *ptr)
{
	if (ptr) {
		Subprocess *psp = static_cast<Subprocess *>(ptr);
		if (!psp->exited) {
#ifdef _WIN32
			::TerminateProcess(psp->pi.hProcess, 1);
			::WaitForSingleObject(psp->pi.hProcess, INFINITE);
#else
			kill(psp->pid, SIGKILL);
#endif
			subprocess_close_process_handle(psp);
			subprocess_close_pipes(psp);
		}
		mrb_free(mrb, ptr);
	}
}

static mrb_value
mrb_subprocess_initialize(mrb_state *mrb, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	if (psp)
		subprocess_free(mrb, psp);

	mrb_data_init(self, NULL, &mrb_subprocess_type);

	mrb_int argc;
	mrb_value *argv;
	mrb_value blk = mrb_nil_value();
	mrb_get_args(mrb, "*&", &argv, &argc, &blk);

	psp = static_cast<Subprocess *>(mrb_malloc(mrb, sizeof(Subprocess)));
	memset(psp, 0, sizeof(*psp));
	psp->mrb = mrb;
	psp->self = self;
	mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "handler"), blk);

#ifdef _WIN32
	hwndSciTE = static_cast<HWND>(host->GetWindowID());
#endif

	if (!subprocess_spawn(mrb, psp, argc, argv)) {
		mrb_free(mrb, psp);
		mrb_raise(mrb, E_RUNTIME_ERROR, "cannnot create process process");
	}

	mrb_data_init(self, psp, &mrb_subprocess_type);

#ifdef _WIN32
	WNDPROC wndproc = reinterpret_cast<WNDPROC>(::GetWindowLongPtr(hwndSciTE, GWLP_WNDPROC));
	if (wndproc != subprocess_subclass_wndproc) {
		org_wndproc = wndproc;
		::SetWindowLongPtr(hwndSciTE, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(subprocess_subclass_wndproc));
	}

	unsigned threadid;
	HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &subprocess_read_output_thread, psp, 0, &threadid);
	CloseHandle(hThread);
#endif

	return self;
}

static mrb_value
mrb_subprocess_pid(mrb_state * /* mrb */, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
#ifdef _WIN32
	return mrb_fixnum_value(psp->pi.dwProcessId);
#else
	return mrb_fixnum_value(psp->pid);
#endif
}

#ifdef _WIN32
BOOL WriteText(HANDLE hStdin, LPCSTR szText, size_t len)
{
	DWORD dwWritten;
	INPUT_RECORD rec = { 0 };
	for (size_t i = 0; i < len; ++i)
	{
		char ch = szText[i];
		if (ch == 10)
			ch = 13;
		rec.EventType = KEY_EVENT;
		rec.Event.KeyEvent.bKeyDown = TRUE;
		rec.Event.KeyEvent.wRepeatCount = 1;
		rec.Event.KeyEvent.uChar.UnicodeChar = ch;
		WriteConsoleInput(hStdin, &rec, 1, &dwWritten);
		rec.Event.KeyEvent.bKeyDown = FALSE;
		WriteConsoleInput(hStdin, &rec, 1, &dwWritten);
	}
	return TRUE;
}
#endif

static mrb_value
mrb_subprocess_send(mrb_state *mrb, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	if (subprocess_pipe_closed(psp))
		mrb_raise(mrb, E_RUNTIME_ERROR, "already closed");
	char *str;
	mrb_int len;
	mrb_get_args(mrb, "s", &str, &len);
#ifdef _WIN32
	HANDLE hConsoleInput = ::GetStdHandle(STD_INPUT_HANDLE);
	::WriteText(hConsoleInput, str, len);
#else
	(void)write(psp->fd_pty_master, str, len);
#endif
	return mrb_nil_value();
}

static mrb_value
mrb_subprocess_recv(mrb_state *mrb, mrb_value self)
{
	char buffer[1024];
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	if (subprocess_pipe_closed(psp))
		mrb_raise(mrb, E_RUNTIME_ERROR, "already closed");
#ifdef _WIN32
	DWORD readbytes;
	if (!ReadFile(psp->hPipeRead, buffer, sizeof(buffer), &readbytes, NULL)) {
		return mrb_nil_value();
	}
#else
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(psp->fd_pty_master, &rfds);
	select(psp->fd_pty_master + 1, &rfds, NULL, NULL, NULL);
	ssize_t readbytes = read(psp->fd_pty_master, buffer, sizeof(buffer));
	if (readbytes <= 0) {
		return mrb_nil_value();
	}
#endif
	return mrb_str_new(mrb, buffer, static_cast<mrb_int>(readbytes));
}

static mrb_value
mrb_subprocess_close(mrb_state * /* mrb */, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	subprocess_close_pipes(psp);
	return mrb_nil_value();
}

static mrb_value
mrb_subprocess_kill(mrb_state *mrb, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	if (psp->exited)
		mrb_raise(mrb, E_RUNTIME_ERROR, "already exited");
#ifdef _WIN32
	TerminateProcess(psp->pi.hProcess, 1);
#else
	kill(psp->pid, SIGKILL);
#endif
	return mrb_nil_value();
}

static mrb_value
mrb_subprocess_exited(mrb_state * /* mrb */, mrb_value self)
{
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	subprocess_update_status(psp);
	if (psp->exited) {
		return mrb_true_value();
	} else {
		return mrb_false_value();
	}
}

static mrb_value
mrb_subprocess_exitstatus(mrb_state *mrb, mrb_value self)
{
	if (!mrb_bool(mrb_subprocess_exited(mrb, self)))
		return mrb_nil_value();
	Subprocess *psp = static_cast<Subprocess*>(DATA_PTR(self));
	return mrb_fixnum_value(psp->exitcode);
}

static void
mrb_subprocess_class_init(mrb_state *mrb, RClass *scite)
{
	RClass *subprocess_class = mrb_define_class_under(mrb, scite, "Subprocess", mrb->object_class);
	MRB_SET_INSTANCE_TT(subprocess_class, MRB_TT_DATA);
	mrb_define_method(mrb, subprocess_class, "initialize", mrb_subprocess_initialize, MRB_ARGS_ANY() | MRB_ARGS_BLOCK());
	mrb_define_method(mrb, subprocess_class, "pid", mrb_subprocess_pid, MRB_ARGS_NONE());
	mrb_define_method(mrb, subprocess_class, "send", mrb_subprocess_send, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, subprocess_class, "recv", mrb_subprocess_recv, MRB_ARGS_NONE());
	mrb_define_method(mrb, subprocess_class, "close", mrb_subprocess_close, MRB_ARGS_NONE());
	mrb_define_method(mrb, subprocess_class, "kill", mrb_subprocess_kill, MRB_ARGS_OPT(1));
	mrb_define_method(mrb, subprocess_class, "exited?", mrb_subprocess_exited, MRB_ARGS_NONE());
	mrb_define_method(mrb, subprocess_class, "exitstatus", mrb_subprocess_exitstatus, MRB_ARGS_NONE());
	mrb_define_const(mrb, subprocess_class, "EVENT_RECV", mrb_fixnum_value(0));
	mrb_define_const(mrb, subprocess_class, "EVENT_EXIT", mrb_fixnum_value(1));
}

/*
static mrb_value mrubyPanicFunction(mrb_state *mrb, mrb_value self) {
	if (mrb == mrbState) {
		mrb_close(mrbState);
		mrbState = NULL;
		mrubyDisabled = true;
	}
	host->Trace("\n> mruby: error occurred in unprotected call.  This is very bad.\n");
	return mrb_nil_value();
}
*/

// Don't initialise Lua in LuaExtension::Initialise.  Wait and initialise Lua the
// first time Lua is used, e.g. when a Load event is called with an argument that
// appears to be the name of a Lua script.  This just-in-time initialisation logic
// does add a little extra complexity but not a lot.  It's probably worth it,
// since it means a user who is having trouble with Lua can just refrain from
// using it.

static bool CheckStartupScript() {
	startupScript = host->Property("ext.mruby.startup.script");
	return startupScript.length() > 0;
}

static void PublishGlobalBufferData() {
	if (curBufferIndex >= 0) {
		mrb_value ary_SciTE_BufferData = mrb_gv_get(mrbState, mrb_intern_lit(mrbState, "SciTE_BufferData_Array"));
		if (!mrb_array_p(ary_SciTE_BufferData)) {
			ary_SciTE_BufferData = mrb_ary_new(mrbState);
			mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "SciTE_BufferData_Array"), ary_SciTE_BufferData);
		}
		mrb_value hash = mrb_ary_entry(ary_SciTE_BufferData, curBufferIndex);
		if (!mrb_hash_p(hash)) {
			// create new buffer-data
			hash = mrb_hash_new(mrbState);
			// remember it
			mrb_ary_set(mrbState, ary_SciTE_BufferData, curBufferIndex, hash);
		}
		// Replace SciTE_BufferData_Array in the stack, leaving (buffer=-1, 'buffer'=-2)
		mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$buffer"), hash);
	} else {
		// for example, during startup, before any InitBuffer / ActivateBuffer
		mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$buffer"), mrb_nil_value());
	}
}

static void backtrace(mrb_state *mrb, const char *error)
{
	SString msg = error ? error : ">mruby: error occurred while loading startup script\n>trace:\n";
	mrb_value ary = mrb_exc_backtrace(mrb, mrb_obj_value(mrb->exc));
	for (mrb_int i = 0; i < RARRAY_LEN(ary); ++i) {
		msg += "\t";
		msg += obj_to_cstr(mrb, mrb_ary_entry(ary, i));
		msg += "\n";
	}
	msg += obj_to_cstr(mrb, mrb_inspect(mrb, mrb_obj_value(mrb->exc)));
	msg += "\n";
	host->Trace(msg.c_str());
}

static bool loadFile(mrb_state *mrb, const char *filename)
{
	mrb_value v = mrb_nil_value();
	FILE *fp = fopen(filename, "r");
	if (!fp) {
		SString msg = ">mruby: error occurred while loading startup script: ";
		msg += filename;
		msg += "\n";
		host->Trace(msg.c_str());
		return false;
	}
	int ai = mrb_gc_arena_save(mrb);
	mrbc_context *ctx = mrbc_context_new(mrb);
	ctx->capture_errors = TRUE;
	mrbc_filename(mrb, ctx, filename);
	mrb_gv_set(mrb, mrb_intern_lit(mrb, "$0"), mrb_str_new_cstr(mrb, filename));
	mrb->exc = NULL;
	v = mrb_load_file_cxt(mrb, fp, ctx);
	mrbc_context_free(mrb, ctx);
	fclose(fp);
	bool result = mrb->exc ? true : false;
	if (mrb->exc) {
		backtrace(mrb);
		mrb->exc = NULL;
	}
	mrb_gc_arena_restore(mrb, ai);
	return result;
}

static bool InitGlobalScope(bool checkProperties, bool forceReload = false) {
	bool reload = forceReload;
	if (checkProperties) {
		int resetMode = GetPropertyInt("ext.mruby.reset");
		if (resetMode >= 1) {
			reload = true;
		}
	}

	tracebackEnabled = (GetPropertyInt("ext.mruby.debug.traceback") == 1);

	if (mrbState) {
		// The Clear / Load used to use metatables to setup without having to re-run the scripts,
		// but this was unreliable e.g. a few library functions and some third-party code use
		// rawget to access functions in the global scope.  So the new method makes a shallow
		// copy of the initialized global environment, and uses that to re-init the scope.

		if (!reload) {
			mrb_value state = mrb_gv_get(mrbState, mrb_intern_lit(mrbState, "SciTE_InitialState"));
			if (!mrb_nil_p(state)) {
				// FIXME:
				//clear_table(mrbState, LUA_GLOBALSINDEX, true);
				//merge_table(mrbState, LUA_GLOBALSINDEX, -1, true);
				//lua_pop(mrbState, 1);

				// restore initial package.loaded state
				// FIXME:
				//lua_getfield(mrbState, LUA_REGISTRYINDEX, "SciTE_InitialPackageState");
				//lua_getfield(mrbState, LUA_REGISTRYINDEX, "_LOADED");
				//clear_table(mrbState, -1, false);
				//merge_table(mrbState, -1, -2, false);
				//lua_pop(mrbState, 2);

				PublishGlobalBufferData();

				return true;
			}
		}

		// reload mode is enabled, or else the initial state has been broken.
		// either way, we're going to need a "new" initial state.

		mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "SciTE_InitialState"), mrb_true_value());

		// Also reset buffer data, since scripts might depend on this to know
		// whether they need to re-initialize something.
		mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "SciTE_BufferData_Array"), mrb_ary_new(mrbState));

		// Don't replace global scope using new_table, because then startup script is
		// bound to a different copy of the globals than the extension script.
		// FIXME:
		//clear_table(mrbState, LUA_GLOBALSINDEX, true);

		// Lua 5.1: _LOADED is in LUA_REGISTRYINDEX, so it must be cleared before
		// loading libraries or they will not load because Lua's package system
		// thinks they are already loaded
		// FIXME:
		//lua_pushnil(mrbState);
		//lua_setfield(mrbState, LUA_REGISTRYINDEX, "_LOADED");

	} else if (!mrubyDisabled) {
		mrbState = mrb_open();
		if (!mrbState) {
			mrubyDisabled = true;
			host->Trace("> mruby: scripting engine failed to initialise\n");
			return false;
		}
		// FIXME:
		//lua_atpanic(mrbState, LuaPanicFunction);

	} else {
		return false;
	}

	// ...register standard libraries
	/*luaL_openlibs(mrbState); */

	// although this is mostly redundant with output:append
	// it is still included for now
	mrb_define_module_function(mrbState, mrbState->kernel_module, "trace", cf_global_trace, MRB_ARGS_REQ(1));

	// emulate a Lua 4 function that is useful in menu commands
	mrb_define_module_function(mrbState, mrbState->kernel_module, "dostring", cf_global_dostring, MRB_ARGS_REQ(1));
	if (!mrb_respond_to(mrbState, mrb_obj_value(mrbState->kernel_module), mrb_intern_lit(mrbState, "eval"))) {
		mrb_define_module_function(mrbState, mrbState->kernel_module, "eval", cf_global_dostring, MRB_ARGS_REQ(1));
	}

	// override a library function whose default impl uses stdout
	mrb_define_module_function(mrbState, mrbState->kernel_module, "__printstr__", cf_global_print_str, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, mrbState->kernel_module, "puts", cf_global_puts, MRB_ARGS_ANY());
	mrb_define_module_function(mrbState, mrbState->kernel_module, "print", cf_global_print, MRB_ARGS_ANY());

	// scite
	RClass *scite = mrb_define_module(mrbState, "SciTE");
	mrb_define_module_function(mrbState, scite, "send_editor", cf_scite_send_editor, MRB_ARGS_ARG(1, 31));
	mrb_define_module_function(mrbState, scite, "send_output", cf_scite_send_output, MRB_ARGS_ARG(1, 31));
	mrb_define_module_function(mrbState, scite, "constant_name", cf_scite_constname, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, scite, "open", cf_scite_open, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, scite, "menu_command", cf_scite_menu_command, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, scite, "update_status_bar", cf_scite_update_status_bar, MRB_ARGS_OPT(1));
	mrb_define_module_function(mrbState, scite, "strip_show_intern", cf_scite_strip_show, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, scite, "strip_set", cf_scite_strip_set, MRB_ARGS_REQ(2));
	mrb_define_module_function(mrbState, scite, "strip_set_list", cf_scite_strip_set_list, MRB_ARGS_REQ(2));
	mrb_define_module_function(mrbState, scite, "strip_value", cf_scite_strip_value, MRB_ARGS_REQ(1));

	// props object - provides access to Property and SetProperty
	RClass *props_module = mrb_define_module_under(mrbState, scite, "Props");
	mrb_define_module_function(mrbState, props_module, "[]",  cf_props_metatable_index, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, props_module, "[]=", cf_props_metatable_newindex, MRB_ARGS_REQ(2));

	mrb_value oprops = mrb_obj_value(props_module);
	mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$props"), oprops);
	mrb_define_global_const(mrbState, "Props", oprops);

	// pane objects
	RClass *pane_class = mrb_define_class_under(mrbState, scite, "Pane", mrbState->object_class);
	MRB_SET_INSTANCE_TT(pane_class, MRB_TT_DATA);
	mrb_define_method(mrbState, pane_class, "method_missing", cf_pane_metatable_index, MRB_ARGS_ARG(1, 2));
	mrb_define_method(mrbState, pane_class, "findtext", cf_pane_findtext, MRB_ARGS_ARG(1, 4));
	mrb_define_method(mrbState, pane_class, "textrange", cf_pane_textrange, MRB_ARGS_REQ(2));
	mrb_define_method(mrbState, pane_class, "insert", cf_pane_insert, MRB_ARGS_REQ(2));
	mrb_define_method(mrbState, pane_class, "remove", cf_pane_remove, MRB_ARGS_REQ(2));
	mrb_define_method(mrbState, pane_class, "append", cf_pane_append, MRB_ARGS_REQ(1));
	mrb_define_method(mrbState, pane_class, "match", cf_pane_match, MRB_ARGS_ARG(1, 3));
	
	// editor
	mrb_value oeditor = create_pane_object(mrbState, ExtensionAPI::paneEditor);
	mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$editor"), oeditor);
	mrb_define_global_const(mrbState, "Editor", oeditor);

	// output
	mrb_value ooutput = create_pane_object(mrbState, ExtensionAPI::paneOutput);
	mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$output"), ooutput);
	mrb_define_global_const(mrbState, "Output", ooutput);

	// StylingContext
	stylingcontext_init(mrbState);

	// PaneMatchObject
	RClass *pane_match_object_class = mrb_define_class_under(mrbState, scite, "PaneMatchObject", mrbState->object_class);
	MRB_SET_INSTANCE_TT(pane_match_object_class, MRB_TT_DATA);
	mrb_include_module(mrbState, pane_match_object_class, mrb_module_get(mrbState, "Enumerable"));
	mrb_define_method(mrbState, pane_match_object_class, "method_missing", cf_match_metatable_index, MRB_ARGS_ARG(1, 2));
	mrb_define_method(mrbState, pane_match_object_class, "each", cf_pane_match_each, MRB_ARGS_BLOCK());
	mrb_define_method(mrbState, pane_match_object_class, "to_s", cf_match_metatable_tostring, MRB_ARGS_NONE());

	// IFacePropertyBinding
	RClass *ifaceprop_class = mrb_define_class_under(mrbState, scite, "IFacePropertyBinding", mrbState->object_class);
	MRB_SET_INSTANCE_TT(ifaceprop_class, MRB_TT_DATA);
	mrb_define_method(mrbState, ifaceprop_class, "[]", cf_ifaceprop_metatable_index, MRB_ARGS_ARG(1, 2));
	mrb_define_method(mrbState, ifaceprop_class, "[]=", cf_ifaceprop_metatable_newindex, MRB_ARGS_ARG(1, 2));

	// Metatable for global namespace, to publish iface constants
	mrb_define_module_function(mrbState, mrbState->object_class, "const_missing", cf_global_metatable_index, MRB_ARGS_REQ(1));
	mrb_define_module_function(mrbState, scite, "const_missing", cf_global_metatable_index, MRB_ARGS_REQ(1));

	// Subprocess class
	mrb_subprocess_class_init(mrbState, scite);

	// scite
	mrb_value oscite = mrb_obj_value(scite);
	mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "$scite"), oscite);

	int ai = mrb_gc_arena_save(mrbState);
	mrb_load_irep(mrbState, mrblib_extman_irep);
	if (mrbState->exc) {
		backtrace(mrbState);
		mrbState->exc = NULL;
	}
	mrb_gc_arena_restore(mrbState, ai);

	if (checkProperties && reload) {
		CheckStartupScript();
	}

	if (startupScript.length()) {
		// TODO: Should buffer be deactivated temporarily, so editor iface
		// functions won't be available during a reset, just as they are not
		// available during a normal startup?  Are there any other functions
		// that should be blocked during startup, e.g. the ones that allow
		// you to add or switch buffers?

		FilePath fpTest(GUI::StringFromUTF8(startupScript.c_str()));
		if (fpTest.Exists()) {
			loadFile(mrbState, startupScript.c_str());
		}
	}

	// Clone the initial state (including metatable) in the registry so that it can be restored.
	// (If reset==1 this will not be used, but this is a shallow copy, not very expensive, and
	// who knows what the value of reset will be the next time InitGlobalScope runs.)
	// FIXME:
	//clone_table(mrbState, LUA_GLOBALSINDEX, true);
	//lua_setfield(mrbState, LUA_REGISTRYINDEX, "SciTE_InitialState");
	mrb_gv_set(mrbState, mrb_intern_lit(mrbState, "SciTE_InitialState"), mrb_true_value());


	// Clone loaded packages (package.loaded) state in the registry so that it can be restored.
	// FIXME:
	//lua_getfield(mrbState, LUA_REGISTRYINDEX, "_LOADED");
	//clone_table(mrbState, -1);
	//lua_setfield(mrbState, LUA_REGISTRYINDEX, "SciTE_InitialPackageState");
	//lua_pop(mrbState, 1);

	PublishGlobalBufferData();

	return true;
}

bool mrubyExtension::Initialise(ExtensionAPI *host_) {
	host = host_;

	if (CheckStartupScript()) {
		InitGlobalScope(false);
	}

	return false;
}

bool mrubyExtension::Finalise() {
	if (mrbState) {
		mrb_close(mrbState);
	}

	mrbState = NULL;
	host = NULL;

	// The rest don't strictly need to be cleared since they
	// are never accessed except when mrbState and host are set

	startupScript = "";

	return false;
}

bool mrubyExtension::Clear() {
	if (mrbState) {
		CallNamedFunction("on_clear");
	}
	if (mrbState) {
		InitGlobalScope(true);
		extensionScript.clear();
	} else if ((GetPropertyInt("ext.mruby.reset") >= 1) && CheckStartupScript()) {
		InitGlobalScope(false);
	}
	return false;
}

bool mrubyExtension::Load(const char *filename) {
	bool loaded = false;

	if (!mrubyDisabled) {
		size_t sl = strlen(filename);
		if (sl >= 3 && strcmp(filename+sl-3, ".rb")==0) {
			if (mrbState || InitGlobalScope(false)) {
				extensionScript = filename;
				loaded = loadFile(mrbState, filename);
			}
		}
	}
	return loaded;
}


bool mrubyExtension::InitBuffer(int index) {
	//char msg[100];
	//sprintf(msg, "InitBuffer(%d)\n", index);
	//host->Trace(msg);

	if (index > maxBufferIndex)
		maxBufferIndex = index;

	if (mrbState) {
		// This buffer might be recycled.  Clear the data associated
		// with the old file.

		mrb_value ary = mrb_gv_get(mrbState, mrb_intern_lit(mrbState, "SciTE_BufferData_Array"));
		if (mrb_array_p(ary))
			mrb_ary_set(mrbState, ary, index, mrb_nil_value());

		// We also need to handle cases where Lua initialization is
		// delayed (e.g. no startup script).  For that we'll just
		// explicitly call InitBuffer(curBufferIndex)
	}

	curBufferIndex = index;

	return false;
}

bool mrubyExtension::ActivateBuffer(int index) {
	//char msg[100];
	//sprintf(msg, "ActivateBuffer(%d)\n", index);
	//host->Trace(msg);

	// Probably don't need to do anything with Lua here.  Setting
	// curBufferIndex is important so that InitGlobalScope knows
	// which buffer is active, in order to populate the 'buffer'
	// global with the right data.

	curBufferIndex = index;

	return false;
}

bool mrubyExtension::RemoveBuffer(int index) {
	//char msg[100];
	//sprintf(msg, "RemoveBuffer(%d)\n", index);
	//host->Trace(msg);

	if (mrbState) {
		// Remove the bufferdata element at index, and move
		// the other elements down.  The element at the
		// current maxBufferIndex can be discarded after
		// it gets copied to maxBufferIndex-1.

		mrb_value ary = mrb_gv_get(mrbState, mrb_intern_lit(mrbState, "SciTE_BufferData_Array"));
		mrb_funcall(mrbState, ary, "delete_at", 1, mrb_fixnum_value(index));
	}

	if (maxBufferIndex > 0)
		maxBufferIndex--;

	// Invalidate current buffer index; Activate or Init will follow.
	curBufferIndex = -1;

	return false;
}

bool mrubyExtension::OnExecute(const char *s) {
	bool handled = false;

	if ((mrbState || InitGlobalScope(false)) && strncmp(s, "lua:", sizeof("lua:") - 1) != 0) {
		// May as well use Lua's pattern matcher to parse the command.
		// Scintilla's RESearch was the other option.

		char *p = new char[strlen(s) + 1];
		char *sclone = p;
		strcpy(p, s);
		if (strncmp(p, "mruby:", sizeof("mruby:") - 1) == 0)
			p += sizeof("mruby:") - 1;
		while (isspace((unsigned char)*p))
			++p;
		char *function = p;
		if (isalpha((unsigned char)*p) || *p == '_') {
			while (*p && !isspace((unsigned char)*p))
				++p;
			if (isspace((unsigned char)*p)) {
				*p++ = 0;
				while (isspace((unsigned char)*p))
					++p;
				char *arg = p;
				p = arg + strlen(arg) - 1;
				while (p > arg && isspace((unsigned char)*p))
					*p-- = 0;

				mrb_sym mid = mrb_intern_cstr(mrbState, function);
				if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
					mrb_value args[] = { mrb_str_new_cstr(mrbState, arg) };
					if (!call_function(mrbState, mid, 1, args, true)) {
						host->Trace("> mruby: error occurred while processing command\n");
					}
				} else {
					host->Trace("> mruby: error checking global scope for command\n");
				}
			}
		}
		delete[] sclone;
	}
	return handled;
}

bool mrubyExtension::OnOpen(const char *filename) {
	return CallNamedFunction("on_open", filename);
}

bool mrubyExtension::OnSwitchFile(const char *filename) {
	return CallNamedFunction("on_switch_file", filename);
}

bool mrubyExtension::OnBeforeSave(const char *filename) {
	return CallNamedFunction("on_before_save", filename);
}

bool mrubyExtension::OnSave(const char *filename) {
	bool result = CallNamedFunction("on_save", filename);

	FilePath fpSaving = FilePath(GUI::StringFromUTF8(filename)).NormalizePath();
	if (startupScript.length() && fpSaving == FilePath(GUI::StringFromUTF8(startupScript.c_str())).NormalizePath()) {
		if (GetPropertyInt("ext.mruby.auto.reload") > 0) {
			InitGlobalScope(false, true);
			if (extensionScript.length()) {
				Load(extensionScript.c_str());
			}
		}
	} else if (extensionScript.length() && 0 == strcmp(filename, extensionScript.c_str())) {
		if (GetPropertyInt("ext.mruby.auto.reload") > 0) {
			InitGlobalScope(false, false);
			Load(extensionScript.c_str());
		}
	}

	return result;
}

bool mrubyExtension::OnChar(char ch) {
	char chs[2] = {ch, '\0'};
	return CallNamedFunction("on_char", chs);
}

bool mrubyExtension::OnSavePointReached() {
	return CallNamedFunction("on_save_point_reached");
}

bool mrubyExtension::OnSavePointLeft() {
	return CallNamedFunction("on_save_point_left");
}

// Similar to StyleContext class in Scintilla
struct StylingContext {
	unsigned int startPos;
	int lengthDoc;
	int initStyle;
	StyleWriter *styler;

	unsigned int endPos;
	unsigned int endDoc;

	unsigned int currentPos;
	bool atLineStart;
	bool atLineEnd;
	int state;

	char cursor[3][8];
	int cursorPos;
	int codePage;
	int lenCurrent;
	int lenNext;

	static StylingContext *Context(mrb_state *mrb, mrb_value self) {
		return static_cast<StylingContext *>(mrb_data_get_ptr(mrb, self, &mrb_sc_type));
	}

	void Colourize() {
		int end = currentPos - 1;
		if (end >= static_cast<int>(endDoc))
			end = static_cast<int>(endDoc)-1;
		styler->ColourTo(end, state);
	}

	static mrb_value Line(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int position;
		mrb_get_args(mrb, "i", &position);
		return mrb_fixnum_value(context->styler->GetLine(position));
	}

	static mrb_value CharAt(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int position;
		mrb_get_args(mrb, "i", &position);
		return mrb_fixnum_value(context->styler->SafeGetCharAt(position));
	}

	static mrb_value StyleAt(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int position;
		mrb_get_args(mrb, "i", &position);
		return mrb_fixnum_value(context->styler->StyleAt(position));
	}

	static mrb_value LevelAt(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int line;
		mrb_get_args(mrb, "i", &line);
		return mrb_fixnum_value(context->styler->LevelAt(line));
	}

	static mrb_value SetLevelAt(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int line, level;
		mrb_get_args(mrb, "ii", &line, &level);
		context->styler->SetLevel(line, level);
		return mrb_nil_value();
	}

	static mrb_value LineState(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int line;
		mrb_get_args(mrb, "i", &line);
		return mrb_fixnum_value(context->styler->GetLineState(line));
	}

	static mrb_value SetLineState(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int line, stateOfLine;
		mrb_get_args(mrb, "ii", &line, &stateOfLine);
		context->styler->SetLineState(line, stateOfLine);
		return mrb_nil_value();
	}


	void GetNextChar() {
		lenCurrent = lenNext;
		lenNext = 1;
		int nextPos = currentPos + lenCurrent;
		unsigned char byteNext = static_cast<unsigned char>(styler->SafeGetCharAt(nextPos));
		unsigned int nextSlot = (cursorPos + 1) % 3;
		memcpy(cursor[nextSlot], "\0\0\0\0\0\0\0\0", 8);
		cursor[nextSlot][0] = byteNext;
		if (codePage) {
			if (codePage == SC_CP_UTF8) {
				if (byteNext >= 0x80) {
					cursor[nextSlot][1] = styler->SafeGetCharAt(nextPos+1);
					lenNext = 2;
					if (byteNext >= 0x80 + 0x40 + 0x20) {
						lenNext = 3;
						cursor[nextSlot][2] = styler->SafeGetCharAt(nextPos+2);
						if (byteNext >= 0x80 + 0x40 + 0x20 + 0x10) {
							lenNext = 4;
							cursor[nextSlot][3] = styler->SafeGetCharAt(nextPos+3);
						}
					}
				}
			} else {
				if (styler->IsLeadByte(byteNext)) {
					lenNext = 2;
					cursor[nextSlot][1] = styler->SafeGetCharAt(nextPos+1);
				}
			}
		}

		// End of line?
		// Trigger on CR only (Mac style) or either on LF from CR+LF (Dos/Win)
		// or on LF alone (Unix). Avoid triggering two times on Dos/Win.
		char ch = cursor[(cursorPos) % 3][0];
		atLineEnd = (ch == '\r' && cursor[nextSlot][0] != '\n') ||
		        (ch == '\n') ||
		        (currentPos >= endPos);
	}

	void StartStyling(mrb_int startPos_, mrb_int length, mrb_int initStyle_) {
		endDoc = styler->Length();
		endPos = startPos_ + length;
		if (endPos == endDoc)
			endPos = endDoc + 1;
		currentPos = startPos_;
		atLineStart = true;
		atLineEnd = false;
		state = initStyle_;
		cursorPos = 0;
		lenCurrent = 0;
		lenNext = 0;
		memcpy(cursor[0], "\0\0\0\0\0\0\0\0", 8);
		memcpy(cursor[1], "\0\0\0\0\0\0\0\0", 8);
		memcpy(cursor[2], "\0\0\0\0\0\0\0\0", 8);
		styler->StartAt(startPos_, static_cast<char>(0xffu));
		styler->StartSegment(startPos_);

		GetNextChar();
		cursorPos++;
		GetNextChar();
	}

	static mrb_value EndStyling(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		context->Colourize();
		return mrb_nil_value();
	}

	static mrb_value StartStyling(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int startPosStyle;
		mrb_int lengthStyle;
		mrb_int initialStyle;
		mrb_get_args(mrb, "iii", &startPosStyle, &lengthStyle, &initialStyle);
		context->StartStyling(startPosStyle, lengthStyle, initialStyle);
		return mrb_nil_value();
	}

	static mrb_value More(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_bool_value(context->currentPos < context->endPos);
	}

	void Forward() {
		if (currentPos < endPos) {
			atLineStart = atLineEnd;
			currentPos += lenCurrent;
			cursorPos++;
			GetNextChar();
		}
		else {
			atLineStart = false;
			memcpy(cursor[0], "\0\0\0\0\0\0\0\0", 8);
			memcpy(cursor[1], "\0\0\0\0\0\0\0\0", 8);
			memcpy(cursor[2], "\0\0\0\0\0\0\0\0", 8);
			atLineEnd = true;
		}
	}

	static mrb_value Forward(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		context->Forward();
		return mrb_nil_value();
	}

	static mrb_value Position(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_fixnum_value(context->currentPos);
	}

	static mrb_value AtLineStart(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_bool_value(context->atLineStart);
	}

	static mrb_value AtLineEnd(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_bool_value(context->atLineEnd);
	}

	static mrb_value State(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_fixnum_value(context->state);
	}

	static mrb_value SetState(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int state;
		mrb_get_args(mrb, "i", &state);
		context->Colourize();
		context->state = state;
		return mrb_nil_value();
	}

	static mrb_value ForwardSetState(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int state;
		mrb_get_args(mrb, "i", &state);
		context->Forward();
		context->Colourize();
		context->state = state;
		return mrb_nil_value();
	}

	static mrb_value ChangeState(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		mrb_int state;
		mrb_get_args(mrb, "i", &state);
		context->state = state;
		return mrb_nil_value();
	}

	static mrb_value Current(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_str_new_cstr(mrb, context->cursor[context->cursorPos % 3]);
	}

	static mrb_value Next(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_str_new_cstr(mrb, context->cursor[(context->cursorPos + 1) % 3]);
	}

	static mrb_value Previous(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		return mrb_str_new_cstr(mrb, context->cursor[(context->cursorPos + 2) % 3]);
	}

	static mrb_value Token(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		int start = context->styler->GetStartSegment();
		int end = context->currentPos - 1;
		int len = end - start + 1;
		mrb_value ret;
		if (len <= 0)
			len = 1;
		char *sReturn = new char[len + 1];
		for (int i = 0; i < len; i++) {
			sReturn[i] = context->styler->SafeGetCharAt(start + i);
		}
		sReturn[len] = '\0';
		ret = mrb_str_new_cstr(mrb, sReturn);
		delete[]sReturn;
		return ret;
	}

	bool Match(const char *s) {
		for (int n=0; *s; n++) {
			if (*s != styler->SafeGetCharAt(currentPos+n))
				return false;
			s++;
		}
		return true;
	}

	static mrb_value Match(mrb_state *mrb, mrb_value self) {
		StylingContext *context = Context(mrb, self);
		const char *s;
		mrb_get_args(mrb, "z", &s);
		return mrb_bool_value(context->Match(s));
	}

	static mrb_value StartPos(mrb_state *mrb, mrb_value self) {
		return mrb_fixnum_value(Context(mrb, self)->startPos);
	}

	static mrb_value LengthDoc(mrb_state *mrb, mrb_value self) {
		return mrb_fixnum_value(Context(mrb, self)->lengthDoc);
	}

	static mrb_value InitStyle(mrb_state *mrb, mrb_value self) {
		return mrb_fixnum_value(Context(mrb, self)->initStyle);
	}

	static mrb_value Create(mrb_state* mrb, int startPos, int lengthDoc, int initStyle, StyleWriter *styler) {
		RClass *styling_context_class = mrb_class_get_under(mrb, M_SCITE, "StylingContext");
		StylingContext *sc = static_cast<StylingContext *>(mrb_malloc(mrb, sizeof(StylingContext)));
		sc->startPos = startPos;
		sc->lengthDoc = lengthDoc;
		sc->initStyle = initStyle;
		sc->styler = styler;
		sc->codePage = static_cast<int>(host->Send(ExtensionAPI::paneEditor, SCI_GETCODEPAGE));

		return mrb_obj_value(mrb_data_object_alloc(mrb, styling_context_class, static_cast<void *>(sc), &mrb_sc_type));
	}

};

static void stylingcontext_init(mrb_state *mrb) {
	RClass *sc_class = mrb_define_class_under(mrb, M_SCITE, "StylingContext", mrb->object_class);
	MRB_SET_INSTANCE_TT(sc_class, MRB_TT_DATA);
	mrb_define_method(mrb, sc_class, "line", StylingContext::Line, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "char_at", StylingContext::Line, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "style_at", StylingContext::StyleAt, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "level_at", StylingContext::LevelAt, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "set_level_at", StylingContext::SetLevelAt, MRB_ARGS_REQ(2));
	mrb_define_method(mrb, sc_class, "line_state", StylingContext::LineState, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "set_line_state", StylingContext::SetLineState, MRB_ARGS_REQ(2));

	mrb_define_method(mrb, sc_class, "start_styling", StylingContext::StartStyling, MRB_ARGS_REQ(3));
	mrb_define_method(mrb, sc_class, "end_styling", StylingContext::EndStyling, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "more", StylingContext::More, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "forward", StylingContext::Forward, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "position", StylingContext::Position, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "at_line_start", StylingContext::AtLineStart, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "at_line_end", StylingContext::AtLineEnd, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "state", StylingContext::State, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "set_state", StylingContext::SetState, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "forward_set_state", StylingContext::ForwardSetState, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "change_state", StylingContext::ChangeState, MRB_ARGS_REQ(1));
	mrb_define_method(mrb, sc_class, "current", StylingContext::Current, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "next", StylingContext::Next, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "previous", StylingContext::Previous, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "token", StylingContext::Token, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "match", StylingContext::Match, MRB_ARGS_REQ(1));

	mrb_define_method(mrb, sc_class, "start_pos", StylingContext::StartPos, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "length_doc", StylingContext::LengthDoc, MRB_ARGS_NONE());
	mrb_define_method(mrb, sc_class, "init_style", StylingContext::InitStyle, MRB_ARGS_NONE());
}

bool mrubyExtension::OnStyle(unsigned int startPos, int lengthDoc, int initStyle, StyleWriter *styler) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_lit(mrbState, "on_style");
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			mrb_value argv[] = { StylingContext::Create(mrbState, startPos, lengthDoc, initStyle, styler) };
			handled = call_function(mrbState, mid, 1, argv);
		}
	}
	return handled;
}

bool mrubyExtension::OnDoubleClick() {
	return CallNamedFunction("on_double_click");
}

bool mrubyExtension::OnUpdateUI() {
	return CallNamedFunction("on_update_ui");
}

bool mrubyExtension::OnMarginClick() {
	return CallNamedFunction("on_margin_click");
}

bool mrubyExtension::OnUserListSelection(int listType, const char *selection) {
	return CallNamedFunction("on_user_list_selection", listType, selection);
}

bool mrubyExtension::OnKey(int keyval, int modifiers) {
	bool handled = false;
	if (mrbState) {
		mrb_sym mid = mrb_intern_lit(mrbState, "on_key");
		if (mrb_respond_to(mrbState, mrb_obj_value(mrbState->top_self), mid)) {
			mrb_value argv[] = {
				mrb_fixnum_value(keyval),
				mrb_bool_value((SCMOD_SHIFT & modifiers) != 0 ? 1 : 0), // shift/lock
				mrb_bool_value((SCMOD_CTRL  & modifiers) != 0 ? 1 : 0), // control
				mrb_bool_value((SCMOD_ALT   & modifiers) != 0 ? 1 : 0)  // alt
			};
			handled = call_function(mrbState, mid, 4, argv);
		}
	}
	return handled;
}

bool mrubyExtension::OnDwellStart(int pos, const char *word) {
	return CallNamedFunction("on_dwell_start", pos, word);
}

bool mrubyExtension::OnClose(const char *filename) {
	return CallNamedFunction("on_close", filename);
}

bool mrubyExtension::OnUserStrip(int control, int change) {
	return CallNamedFunction("on_strip", control, change);
}


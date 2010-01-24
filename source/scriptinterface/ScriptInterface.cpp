/* Copyright (C) 2009 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "ScriptInterface.h"

#include "lib/debug.h"
#include "ps/CLogger.h"
#include "ps/utf16string.h"

#include <cassert>

#include "js/jsapi.h"

#include <boost/preprocessor/punctuation/comma_if.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>

#ifdef USE_VALGRIND
# include <valgrind/valgrind.h>
#endif

const int RUNTIME_SIZE = 4 * 1024 * 1024; // TODO: how much memory is needed?
const int STACK_CHUNK_SIZE = 8192;

////////////////////////////////////////////////////////////////

struct ScriptInterface_impl
{
	ScriptInterface_impl(const char* nativeScopeName, JSContext* cx);
	~ScriptInterface_impl();
	void Register(const char* name, JSNative fptr, uintN nargs);

	JSRuntime* m_rt; // NULL if m_cx is shared; non-NULL if we own m_cx
	JSContext* m_cx;
	JSObject* m_glob; // global scope object
	JSObject* m_nativeScope; // native function scope object
};

namespace
{

JSClass global_class = {
	"global", JSCLASS_GLOBAL_FLAGS,
	JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL
};

void ErrorReporter(JSContext* UNUSED(cx), const char* message, JSErrorReport* report)
{
	std::stringstream msg;
	bool isWarning = JSREPORT_IS_WARNING(report->flags);
	msg << (isWarning ? "JavaScript warning: " : "JavaScript error: ");
	if (report->filename)
	{
		msg << report->filename;
		msg << " line " << report->lineno << "\n";
	}
	msg << message;
	if (isWarning)
		LOGWARNING(L"%hs", msg.str().c_str());
	else
		LOGERROR(L"%hs", msg.str().c_str());
#ifdef USE_VALGRIND
	// When running under Valgrind, print more information in the error message
	VALGRIND_PRINTF_BACKTRACE("->");
#endif
}

// Functions in the global namespace:

JSBool print(JSContext* cx, JSObject* UNUSED(obj), uintN argc, jsval* argv, jsval* UNUSED(rval))
{
	for (uintN i = 0; i < argc; ++i)
	{
		std::string str;
		if (!ScriptInterface::FromJSVal(cx, argv[i], str))
			return JS_FALSE;
		printf("%s", str.c_str());
	}
	fflush(stdout);
	return JS_TRUE;
}

} // anonymous namespace

ScriptInterface_impl::ScriptInterface_impl(const char* nativeScopeName, JSContext* cx)
{
	JSBool ok;

	if (cx)
	{
		m_rt = NULL;
		m_cx = cx;
		m_glob = JS_GetGlobalObject(m_cx);
	}
	else
	{
		m_rt = JS_NewRuntime(RUNTIME_SIZE);
		debug_assert(m_rt); // TODO: error handling

		m_cx = JS_NewContext(m_rt, STACK_CHUNK_SIZE);
		debug_assert(m_cx);

		// For GC debugging with SpiderMonkey 1.8+:
		// JS_SetGCZeal(m_cx, 2);

		JS_SetContextPrivate(m_cx, NULL);

		JS_SetErrorReporter(m_cx, ErrorReporter);

		JS_SetOptions(m_cx, JSOPTION_STRICT // "warn on dubious practice"
				| JSOPTION_XML // "ECMAScript for XML support: parse <!-- --> as a token"
				| JSOPTION_VAROBJFIX // "recommended" (fixes variable scoping)
		);

		// Threadsafe SpiderMonkey requires that we have a request before doing anything much
		JS_BeginRequest(m_cx);

		m_glob = JS_NewObject(m_cx, &global_class, NULL, NULL);
		ok = JS_InitStandardClasses(m_cx, m_glob);

		JS_DefineProperty(m_cx, m_glob, "global", OBJECT_TO_JSVAL(m_glob), NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY
				| JSPROP_PERMANENT);
	}

	m_nativeScope = JS_DefineObject(m_cx, m_glob, nativeScopeName, NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY
			| JSPROP_PERMANENT);

	JS_DefineFunction(m_cx, m_glob, "print", ::print, 0, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT);
}

ScriptInterface_impl::~ScriptInterface_impl()
{
	if (m_rt) // if we own the context:
	{
		JS_EndRequest(m_cx);
		JS_DestroyContext(m_cx);
		JS_DestroyRuntime(m_rt);
	}
}

static JSBool LoadScript(JSContext* cx, const jschar* chars, uintN length, const char* filename, jsval* rval)
{
	JSFunction* func = JS_CompileUCFunction(cx, NULL, NULL, 0, NULL, chars, length, filename, 1);
	if (!func)
		return JS_FALSE;
	JS_AddRoot(cx, &func); // TODO: do we need to root this?
	*rval = OBJECT_TO_JSVAL((JSObject*)func);
	jsval scriptRval;
	JSBool ok = JS_CallFunction(cx, NULL, func, 0, NULL, &scriptRval);
	JS_RemoveRoot(cx, &func);
	return ok;
}

void ScriptInterface_impl::Register(const char* name, JSNative fptr, uintN nargs)
{
	JS_DefineFunction(m_cx, m_nativeScope, name, fptr, nargs, JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT);
}

ScriptInterface::ScriptInterface(const char* nativeScopeName, JSContext* cx) :
	m(new ScriptInterface_impl(nativeScopeName, cx))
{
}

ScriptInterface::~ScriptInterface()
{
}

void ScriptInterface::ShutDown()
{
	JS_ShutDown();
}

void ScriptInterface::SetCallbackData(void* cbdata)
{
	JS_SetContextPrivate(m->m_cx, cbdata);
}

void* ScriptInterface::GetCallbackData(JSContext* cx)
{
	return JS_GetContextPrivate(cx);
}

void ScriptInterface::Register(const char* name, JSNative fptr, size_t nargs)
{
	m->Register(name, fptr, (uintN)nargs);
}

JSContext* ScriptInterface::GetContext()
{
	return m->m_cx;
}

bool ScriptInterface::AddRoot(void* ptr, const char* name)
{
	return JS_AddNamedRoot(m->m_cx, ptr, name) ? true : false;
}

bool ScriptInterface::RemoveRoot(void* ptr)
{
	return JS_RemoveRoot(m->m_cx, ptr) ? true : false;
}

ScriptInterface::LocalRootScope::LocalRootScope(JSContext* cx) :
	m_cx(cx)
{
	m_OK = JS_EnterLocalRootScope(m_cx) ? true : false;
}

ScriptInterface::LocalRootScope::~LocalRootScope()
{
	if (m_OK)
		JS_LeaveLocalRootScope(m_cx);
}

bool ScriptInterface::LocalRootScope::OK()
{
	return m_OK;
}

jsval ScriptInterface::CallConstructor(jsval ctor)
{
	// Constructing JS objects similarly to "new Foo" is non-trivial.
	// https://developer.mozilla.org/En/SpiderMonkey/JSAPI_Phrasebook#Constructing_an_object_with_new
	// suggests some ugly ways, so we'll use a different way that's less compatible but less ugly

	if (!(JSVAL_IS_OBJECT(ctor) && JS_ObjectIsFunction(m->m_cx, JSVAL_TO_OBJECT(ctor))))
	{
		LOGERROR(L"CallConstructor: ctor is not a function object");
		return JSVAL_VOID;
	}

	// Get the constructor's prototype
	// (Can't use JS_GetPrototype, since we want .prototype not .__proto__)
	jsval protoVal;
	if (!JS_GetProperty(m->m_cx, JSVAL_TO_OBJECT(ctor), "prototype", &protoVal))
	{
		LOGERROR(L"CallConstructor: can't get prototype");
		return JSVAL_VOID;
	}

	if (!JSVAL_IS_OBJECT(protoVal))
	{
		LOGERROR(L"CallConstructor: prototype is not an object");
		return JSVAL_VOID;
	}

	JSObject* proto = JSVAL_TO_OBJECT(protoVal);
	JSObject* parent = JS_GetParent(m->m_cx, JSVAL_TO_OBJECT(ctor));
	// TODO: rooting?
	if (!proto || !parent)
	{
		LOGERROR(L"CallConstructor: null proto/parent");
		return JSVAL_VOID;
	}

	JSObject* obj = JS_NewObject(m->m_cx, NULL, proto, parent);
	if (!obj)
	{
		LOGERROR(L"CallConstructor: object creation failed");
		return JSVAL_VOID;
	}

	jsval rval;
	if (!JS_CallFunctionValue(m->m_cx, obj, ctor, 0, NULL, &rval))
	{
		LOGERROR(L"CallConstructor: ctor failed");
		return JSVAL_VOID;
	}

	return OBJECT_TO_JSVAL(obj);
}

bool ScriptInterface::CallFunctionVoid(jsval val, const char* name)
{
	jsval jsRet;
	std::vector<jsval> argv;
	return CallFunction_(val, name, argv, jsRet);
}

bool ScriptInterface::CallFunction_(jsval val, const char* name, std::vector<jsval>& args, jsval& ret)
{
	const uintN argc = args.size();
	jsval* argv = NULL;
	if (argc)
		argv = &args[0];

	JSObject* obj;
	if (!JS_ValueToObject(m->m_cx, val, &obj) || obj == NULL)
		return false;
	JS_AddRoot(m->m_cx, &obj);

	// Check that the named function actually exists, to avoid ugly JS error reports
	// when calling an undefined value
	JSBool found;
	if (!JS_HasProperty(m->m_cx, obj, name, &found) || !found)
	{
		JS_RemoveRoot(m->m_cx, &obj);
		return false;
	}

	JSBool ok = JS_CallFunctionName(m->m_cx, obj, name, argc, argv, &ret);
	JS_RemoveRoot(m->m_cx, &obj);

	return ok ? true : false;
}

jsval ScriptInterface::GetGlobalObject()
{
	return OBJECT_TO_JSVAL(JS_GetGlobalObject(m->m_cx));
}

bool ScriptInterface::SetGlobal_(const char* name, jsval value, bool replace)
{
	if (!replace)
	{
		JSBool found;
		if (!JS_HasProperty(m->m_cx, m->m_glob, name, &found))
			return false;
		if (found)
		{
			JS_ReportError(m->m_cx, "SetGlobal \"%s\" called multiple times", name);
			return false;
		}
	}

	JSBool ok = JS_DefineProperty(m->m_cx, m->m_glob, name, value, NULL, NULL, JSPROP_ENUMERATE | JSPROP_READONLY
			| JSPROP_PERMANENT);
	return ok ? true : false;
}

bool ScriptInterface::SetProperty_(jsval obj, const char* name, jsval value, bool constant)
{
	uintN attrs;
	if (constant)
		attrs = JSPROP_READONLY | JSPROP_PERMANENT;
	else
		attrs = JSPROP_ENUMERATE;

	if (! JSVAL_IS_OBJECT(obj))
		return false;
	JSObject* object = JSVAL_TO_OBJECT(obj);

	if (! JS_DefineProperty(m->m_cx, object, name, value, NULL, NULL, attrs))
		return false;
	return true;
}

bool ScriptInterface::GetProperty_(jsval obj, const char* name, jsval& out)
{
	if (! JSVAL_IS_OBJECT(obj))
		return false;
	JSObject* object = JSVAL_TO_OBJECT(obj);

	if (!JS_GetProperty(m->m_cx, object, name, &out))
		return false;
	return true;
}

bool ScriptInterface::EnumeratePropertyNamesWithPrefix(jsval obj, const char* prefix, std::vector<std::string>& out)
{
	LOCAL_ROOT_SCOPE;

	utf16string prefix16 (prefix, prefix+strlen(prefix));

	if (! JSVAL_IS_OBJECT(obj))
		return false; // TODO: log error messages

	JSObject* it = JS_NewPropertyIterator(m->m_cx, JSVAL_TO_OBJECT(obj));
	if (!it)
		return false;

	while (true)
	{
		jsid idp;
		jsval val;
		if (! JS_NextProperty(m->m_cx, it, &idp) || ! JS_IdToValue(m->m_cx, idp, &val))
			return false;
		if (val == JSVAL_VOID)
			break; // end of iteration
		if (! JSVAL_IS_STRING(val))
			continue; // ignore integer properties

		JSString* name = JSVAL_TO_STRING(val);
		size_t len = JS_GetStringLength(name);
		jschar* chars = JS_GetStringChars(name);
		if (len >= prefix16.size() && memcmp(chars, prefix16.c_str(), prefix16.size()*2) == 0)
			out.push_back(std::string(chars, chars+len)); // handles Unicode poorly
	}

	// Recurse up the prototype chain
	JSObject* prototype = JS_GetPrototype(m->m_cx, JSVAL_TO_OBJECT(obj));
	if (prototype)
	{
		if (! EnumeratePropertyNamesWithPrefix(OBJECT_TO_JSVAL(prototype), prefix, out))
			return false;
	}

	return true;
}

bool ScriptInterface::SetPrototype(jsval obj, jsval proto)
{
	if (!JSVAL_IS_OBJECT(obj) || !JSVAL_IS_OBJECT(proto))
		return false;
	return JS_SetPrototype(m->m_cx, JSVAL_TO_OBJECT(obj), JSVAL_TO_OBJECT(proto)) ? true : false;
}

bool ScriptInterface::LoadScript(const std::wstring& filename, const std::wstring& code)
{
	std::string fnAscii(filename.begin(), filename.end());
	utf16string codeUtf16(code.begin(), code.end());
	jsval rval;
	JSBool ok = ::LoadScript(m->m_cx, reinterpret_cast<const jschar*> (codeUtf16.c_str()), (uintN)(codeUtf16.length()),
			fnAscii.c_str(), &rval);
	return ok ? true : false;
}

bool ScriptInterface::Eval(const char* code)
{
	jsval rval;
	return Eval_(code, rval);
}

bool ScriptInterface::Eval_(const char* code, jsval& rval)
{
	utf16string codeUtf16(code, code+strlen(code));

	JSBool ok = JS_EvaluateUCScript(m->m_cx, m->m_glob, (const jschar*)codeUtf16.c_str(), (uintN)codeUtf16.length(), "(eval)", 1, &rval);
	return ok ? true : false;
}

void ScriptInterface::ReportError(const char* msg)
{
	// JS_ReportError by itself doesn't seem to set a JS-style exception, and so
	// script callers will be unable to catch anything. So use JS_SetPendingException
	// to make sure there really is a script-level exception. But just set it to undefined
	// because there's not much value yet in throwing a real exception object.
	JS_SetPendingException(m->m_cx, JSVAL_VOID);
	// And report the actual error
	JS_ReportError(m->m_cx, "%s", msg);

	// TODO: Why doesn't JS_ReportPendingException(m->m_cx); work?
}

bool ScriptInterface::IsExceptionPending(JSContext* cx)
{
	return JS_IsExceptionPending(cx) ? true : false;
}

JSClass* ScriptInterface::GetClass(JSContext* cx, JSObject* obj)
{
	return JS_GetClass(cx, obj);
}

void* ScriptInterface::GetPrivate(JSContext* cx, JSObject* obj)
{
	// TODO: use JS_GetInstancePrivate
	return JS_GetPrivate(cx, obj);
}

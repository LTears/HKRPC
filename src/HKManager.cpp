#include "HKManager.h"
#include "HKServer.h"
#include "HKHook.h"
#include "Utils.h"
#include <unordered_map>

#pragma comment(lib, "version.lib")


class Arguments {
public:
	intptr_t* GetValues() {
		return values_.data();
	}

	int GetCount() {
		return values_.size();
	}

	bool AddValue(json_value* val, const String& type) {
		if (val->type == json_string) {
			const char* u8str = val->u.string.ptr;
			if (_stricmp(type.c_str(), "wstring")==0) { //wstring
				WString wstr = Utils::ToUTF16(u8str);
				wstrholder_.push_back(wstr);
				const wchar_t* pwstr = wstrholder_.back().c_str();
				values_.push_back((intptr_t)pwstr);
			}
			else if (_stricmp(type.c_str(), "astring")==0) { //astring
				WString wstr = Utils::ToUTF16(u8str);
				String astr = Utils::ToANSI(wstr);
				strholder_.push_back(astr);
				const char* pastr = strholder_.back().c_str();
				values_.push_back((intptr_t)pastr);
			}
			else { //string
				values_.push_back((intptr_t)u8str);
			}
		}
		else if (val->type == json_integer) {
			values_.push_back((intptr_t)val->u.integer);
		}
		else if (val->type == json_boolean) {
			values_.push_back(val->u.boolean ? 1 : 0);
		}
		else if (val->type == json_null) {
			values_.push_back(0);
		}
		else {
			return false;
		}
		return true;
	}

protected:
	std::list<String> strholder_;
	std::list<WString> wstrholder_;
	std::vector<intptr_t> values_;
};


//�Ự����
class SessionData {
public:
	SessionData() {

	}
	~SessionData() {
		RemoveAllHookFilter();
	}

	bool IsHookFilterExist(HKHookFilter* filter) {
		auto it = filters_.find(filter);
		return it != filters_.end();
	}

	void AddHookAddrFilter(HKHookFilter* filter) {
		filters_.insert(filter);
	}
	void RemoveHookFilter(HKHookFilter* filter) {
		filters_.erase(filter);
		delete filter;
	}
	void RemoveAllHookFilter() {
		for (auto filter : filters_) {
			delete filter;
		}
		filters_.clear();
	}
protected:
	std::set<HKHookFilter*> filters_;
};


static HKServer* server_; //ȫ��server����
static std::unordered_map<ConnectionPtr, Buffer> tempbufs_; //��ʱ�����������ڴ���ÿ�����ӵ�ճ�������
typedef std::set<HKHookFilter*> HookFilterSet;
static std::unordered_map<void*, HKHook*> addrhook_; //��ַ��Ӧ��hook һ��һ
static std::unordered_map<ConnectionPtr, SessionData*> sessions_; //���Ӷ�Ӧ��session
typedef String(*RequestProcessMethod)(ConnectionPtr, json_value*, json_value*);
static std::unordered_map<String, RequestProcessMethod> methods_; //Э�鴦������


static void SendRsponse(ConnectionPtr conn, const String& str) {
	Buffer buf;
	buf = "0000" + str;
	unsigned char* buffer = (unsigned char*)buf.data();
	Utils::WriteIntBE(buffer, str.length());
	buffer[0] = 0xF8;
	buffer[1] = 0xFB;
	server_->sendData(conn, buf);
}



//JSON�������л�
static String JsonSerialize(json_value* value) {
	if (!value)return "null";
	String result;
	if (value->type == json_string) {
		result += "\"";
		result += Utils::EncodeString(value->u.string.ptr);
		result += "\"";
	}
	else if (value->type == json_integer) {
		result += std::to_string(value->u.integer);
	}
	else if (value->type == json_double) {
		result += std::to_string(value->u.dbl);
	}
	else if (value->type == json_boolean) {
		result += value->u.boolean ? "true" : "false";
	}
	else if (value->type == json_null) {
		result += "null";
	}
	else if (value->type == json_object) {
		int len = value->u.object.length;
		result += "{";
		for (int i = 0; i < len; i++) {
			if (i > 0)result += ",";
			auto entry = value->u.object.values[i];
			result += "\"";
			result += entry.name;
			result += "\":\"";
			result += JsonSerialize(entry.value);
		}
		result += "}";
	}
	else if (value->type == json_array) {
		int len = value->u.array.length;
		result += "[";
		for (int i = 0; i < len; i++) {
			if (i > 0)result += ",";
			auto v = value->u.array.values[i];
			result += JsonSerialize(v);
		}
		result += "]";
	}
	return result;
}



//����result��Ӧ
static String MakeResultResponse(const char* jsonresult, json_value* id) {
	String resp;
	resp += "{\"result\":";
	resp += jsonresult;
	resp += ",\"id\":";
	resp += JsonSerialize(id);
	resp += "}";
	return resp;
}

//����error��Ӧ
static String MakeErrorResponse(int code, const char* msg, json_value* id = NULL) {
	String resp;
	resp += "{\"error\":{\"code\":";
	resp += std::to_string(code);
	resp += ",\"message\":\"";
	resp += Utils::EncodeString(msg);
	resp += "\"},\"id\":";
	resp += JsonSerialize(id);	
	resp += "}";
	return resp;
}





//����echo����
String ProcessEchoRequest(ConnectionPtr conn, json_value* id, json_value* params) {
	String result = JsonSerialize(params);
	return MakeResultResponse(result.c_str(), id);
}

//��ȡdll�汾
String ProcessModuleVersionRequest(ConnectionPtr conn, json_value* id, json_value* params) {
	int argc = params->u.array.length;
	if (argc < 1)return MakeErrorResponse(-32600, "Invalid Request, no enough params", id);
	const char* mod = *params->u.array.values[0];
	String ver = Utils::GetModuleVersion(mod);
	if (ver.empty()) {
		return MakeErrorResponse(-1, "can not get version " , id);
	}
	String result;
	result += "\"";
	result += ver;
	result += "\"";
	return MakeResultResponse(result.c_str(), id);
}

//hook|callЭ���У���ȡ��ֵַ
void* GetAddressParam(json_value* params) {
	void* addr = NULL;
	const char* mod = *(params->u.array.values[0]);
	int targettype = params->u.array.values[1]->type;
	if (targettype == json_string) { //��������hook
		const char* func = *(params->u.array.values[1]);
		addr = Utils::GetFunctionAddress(mod, func);
	}
	else if (targettype == json_integer) { //����ַhook
		json_int_t addroffset = *(params->u.array.values[1]);
		void* base = Utils::GetModuleBaseAddress(mod);
		addr = (char*)base + addroffset;
	}
	return addr;
}


//����hook���� (dll, target, types, [offsets])
String ProcessHookRequest(ConnectionPtr conn, json_value* id, json_value* params) {
	int argc = params->u.array.length;
	if (argc < 3)return MakeErrorResponse(-32600, "Invalid Request, no enough params", id);

	//��ȡĿ���ַ
	void* addr = GetAddressParam(params);
	if (!addr) {
		return MakeErrorResponse(-32600, "Invalid Request, target address not found", id);
	}
	//types, offset ������֤
	json_value* types = params->u.array.values[2];
	if (types->type != json_array)return MakeErrorResponse(-32600, "Invalid Request, param types is not array", id);
	json_value* offsets = NULL;
	if (argc >= 4) {
		offsets = params->u.array.values[3];
		if (offsets->type != json_array)return MakeErrorResponse(-32600, "Invalid Request, param offsets is not array", id);
		if (offsets->u.array.length < types->u.array.length)return MakeErrorResponse(-32600, "Invalid Request, offsets.length must equal types.length", id);
	}

	//����hook
	HKHook* hook = NULL;
	auto it = addrhook_.find(addr); //��Ѱ�Ҹõ�ַ�Ƿ��Ѿ�����hook
	if (it != addrhook_.end()) {
		hook = it->second;
	}
	if (!hook) {
		//�õ�ַû��hook�����½�
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		hook = new HKHook();
		if (!hook->Attach(addr)) {
			return MakeErrorResponse(-1, "Hook Attach Error", id);
		}
		int ret = DetourTransactionCommit();
		if (ret) {
			return MakeErrorResponse(ret, "Hook Commit Error", id);
		}
	}

	//����hook����������
	HKHookFilter* filter = new HKHookFilter();
	if (types) {
		int count = types->u.array.length;
		for (int i = 0; i < count; i++) {
			const char* val = *types->u.array.values[i];
			filter->AddParamType(val);
		}
	}
	if (offsets) {
		int count = offsets->u.array.length;
		for (int i = 0; i < count; i++) {
			int val = (int)(json_int_t)*offsets->u.array.values[i];
			filter->AddParamOffset(val);
		}
	}
	else {
		filter->MakeDefaultOffset();
	}

	String hookid = std::to_string((uintptr_t)filter);
	String reqid = JsonSerialize(id);
	//���ù������������������hook����������ʱ��֪ͨ�ͻ��ˣ�
	filter->SerResultHandler([=](const String& ret) {
		String result;
		result += "{\"method\":\"hook_notify\",\"params\":";
		result += ret;
		result += ",\"requestid\":";
		result += reqid;
		result += "}";
		SendRsponse(conn, result);
	});

	//��ӵ�session��
	hook->AddFilter(filter);
	auto session = sessions_[conn];
	if (session)session->AddHookAddrFilter(filter);
	return MakeResultResponse(hookid.c_str(), id);
}


//ɾ��hook
String ProcessHookDeleteRequest(ConnectionPtr conn, json_value* id, json_value* params) {
	int argc = params->u.array.length;
	if (argc < 1)return MakeErrorResponse(-32600, "Invalid Request, no enough params", id);
	uintptr_t val = (uintptr_t)(json_int_t)*params->u.array.values[0];
	HKHookFilter* filter = (HKHookFilter*)val;
	auto session = sessions_[conn];
	if (session) {
		if (session->IsHookFilterExist(filter)) {
			session->RemoveHookFilter(filter);
			return MakeResultResponse("ok", id);
		}
	}
	return MakeErrorResponse(-1, "hook not found");
}


static bool SafeCall(const char* calltype, void* addr, int argc, intptr_t* args, intptr_t* ret) {
	intptr_t funcret = 0;
	bool result = false;
	__try {
		if (strcmp(calltype, "cdecl") == 0) {
			funcret = mhcode_call_cdecl(addr, argc, args);
		}
		else if (strcmp(calltype, "stdcall") == 0) {
			funcret = mhcode_call_stdcall(addr, argc, args);
		}
		else if (strcmp(calltype, "thiscall") == 0) {
			funcret = mhcode_call_thiscall(addr, argc, args);
		}
		result = true;
		*ret = funcret;
	}
	__finally {		
	}
	return result;
}

//����call  (dll, target, calltype, args, argtypes, rettype)
String ProcessCallRequest(ConnectionPtr conn, json_value* id, json_value* params) {
	int paramc = params->u.array.length;
	if (paramc < 4)return MakeErrorResponse(-32600, "Invalid Request, no enough params", id);
	//��ȡ������ַ
	void* addr = GetAddressParam(params);
	if(!addr)return MakeErrorResponse(-1, "Function not found", id);
	//��ȡ������
	const char* calltype = *params->u.array.values[2];
	json_value* args = params->u.array.values[3];
	if (args->type != json_array)return MakeErrorResponse(-32600, "Invalid Request, param args is not array", id);
	int argc = args->u.array.length;
	json_value* argtypes = NULL;
	if (paramc >= 5) {
		argtypes = params->u.array.values[4];
		if (argtypes->type != json_array)return MakeErrorResponse(-32600, "Invalid Request, param argtypes is not array", id);
		if (argtypes->u.array.length < args->u.array.length)return MakeErrorResponse(-32600, "Invalid Request, args.length must equal argtypes.length", id);
	}
	const char* rettype = "intptr";
	if (paramc >= 6) rettype = *params->u.array.values[5];

	//��װ���������������
	Arguments arg;
	bool addret = true;
	for (int i = 0; i < argc; i++) {
		if (argtypes) {
			const char* type = *argtypes->u.array.values[i];
			addret = arg.AddValue(args->u.array.values[i], type);
		}
		else {
			addret = arg.AddValue(args->u.array.values[i], "");
		}
		if (!addret) {
			return MakeErrorResponse(-1, "unknown argument type", id);
		}
	}
	//���к�������
	intptr_t funcret = 0;
	bool callok = SafeCall(calltype, addr, arg.GetCount(), arg.GetValues(), &funcret);
	if (!callok) {
		return MakeErrorResponse(-1, "call error");
	}

	//���ؽ��
	String ret = Utils::ToJsonFormat(funcret, rettype);
	return MakeResultResponse(ret.c_str(), id);

}


//���������󣬷�����Ӧ����
String ProcessSingleRequest(ConnectionPtr conn, json_value* json) {
	const char* method = (*json)["method"];
	const json_value& id = (*json)["id"];
	auto params = (*json)["params"];
	if (!method || params.type != json_array) {
		return MakeErrorResponse(-32600, "Invalid Request", (json_value*)&id);
	}
	auto it = methods_.find(method);
	if (it != methods_.end()) {
		return it->second(conn, (json_value*)&id, &params);
	}
	return MakeErrorResponse(-32601, "Method not found", (json_value*)&id);
}

//����batch���󣬷�����Ӧ����
String ProcessBatchRequest(ConnectionPtr conn, json_value* json) {
	String result;
	return MakeErrorResponse(-1, "not support batch request");;
}

//�յ��ͻ��˰�
static void OnClientPacket(ConnectionPtr conn, const char* body, int bodylen) {
	String data;
	data.assign(body, bodylen);
	printf("recv %d bytes:%s\n", bodylen, data.c_str());

	char errbuf[256];
	errbuf[0] = 0;
	json_settings settings = { 0 };
	json_value* value = json_parse_ex(&settings, body, bodylen, errbuf);
	if (!value) {
		SendRsponse(conn, MakeErrorResponse(-32700, errbuf));		
	}
	else {
		String ret;
		if (value->type == json_array) {
			ret = ProcessBatchRequest(conn, value);
		}
		else {
			ret = ProcessSingleRequest(conn, value);
		}
		SendRsponse(conn, ret);
		json_value_free(value);
	}
}


//���Ӵ���ʱ������session
static void OnClientConnect(ConnectionPtr conn) {
	auto session = new SessionData();
	sessions_[conn] = session;
}

//���ӹر�ʱ������session
static void OnClientClose(ConnectionPtr conn) {
	auto session = sessions_[conn];
	if (session) {
		delete session;
	}
	sessions_.erase(conn);
}

//�յ�����ʱ������ճ������ȣ��Ե��������� OnClientPacket
static void OnClientData(ConnectionPtr conn, const Buffer& data) {
	Buffer buf = tempbufs_[conn] + data;
	uint32_t remainlen = buf.length();
	int packetstart = 0;
	bool haspack = false;
	while (remainlen > 4) {
		uint32_t head = Utils::ReadIntBE(buf.data() + packetstart);
		uint32_t magic = (head >> 16);
		uint32_t packlen = (head & 0xffff);
		if (magic != 0xF8FB) { //magic error
			//server_->close(conn);
			tempbufs_[conn].clear();
			return;
		}
		packlen += 4;
		if (remainlen >= packlen) {
			OnClientPacket(conn, buf.data() + packetstart + 4, packlen - 4);
			remainlen -= packlen;
			packetstart += packlen;
			haspack = true;
		}
	}
	if (haspack) {
		if (remainlen) {
			tempbufs_[conn] = buf.substr(packetstart);
		}
		else {
			tempbufs_[conn].clear();
		}
	}
}

bool HKManager::init() {
	//ע��Э�鴦����
	methods_["echo"] = ProcessEchoRequest;
	methods_["hook"] = ProcessHookRequest;
	methods_["hook_delete"] = ProcessHookDeleteRequest;
	methods_["call"] = ProcessCallRequest;
	methods_["module_version"] = ProcessModuleVersionRequest;

	//��������
	server_ = HKServer::getInstance();
	server_->setOnData(OnClientData);
	server_->setOnClose(OnClientClose);
	server_->setOnConnect(OnClientConnect);
	server_->start();
	return true;
}


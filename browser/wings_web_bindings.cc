#include "wings/browser/wings_web_bindings.h"
#include "wings/browser/wings_web_frontend_host.h"

#include <stddef.h>

#include <utility>

#include "base/guid.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_client.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_browser_context.h"
#include "content/shell/browser/shell_browser_main_parts.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "content/shell/browser/shell_devtools_manager_delegate.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_fetcher.h"
#include "net/url_request/url_fetcher_response_writer.h"

#if !defined(OS_ANDROID)
#include "content/public/browser/devtools_frontend_host.h"
#endif

namespace wings {

namespace {

} // namespace

// This constant should be in sync with
// the constant at devtools_ui_bindings.cc.
const size_t kMaxMessageChunkSize = IPC::Channel::kMaximumMessageSize / 4;

WingsWebBindings::WingsWebBindings(content::WebContents* devtools_contents,
                      content::WebContents* inspected_contents)
  : content::WebContentsObserver(devtools_contents),
    inspected_contents_(inspected_contents),
    inspect_element_at_x_(-1),
    inspect_element_at_y_(-1),
    weak_factory_(this) {
}

WingsWebBindings::~WingsWebBindings() {
  for (const auto& pair : pending_requests_)
    delete pair.first;
  if (agent_host_)
    agent_host_->DetachClient(this);  
}

void WingsWebBindings::InspectElementAt(int x, int y) {
  LOG(INFO) << "unimplemented";
}

void WingsWebBindings::Attach() {
  if (agent_host_)
    agent_host_->DetachClient(this);
  agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(inspected_contents_);
  agent_host_->AttachClient(this);
  if (inspect_element_at_x_ != -1) {
    agent_host_->InspectElement(inspected_contents_->GetFocusedFrame(),
                                inspect_element_at_x_, inspect_element_at_y_);
    inspect_element_at_x_ = -1;
    inspect_element_at_y_ = -1;
  }
}

void WingsWebBindings::CallClientFunction(const std::string& function_name,
                        const base::Value* arg1,
                        const base::Value* arg2,
                        const base::Value* arg3) {
  std::string javascript = function_name + "(";
  if (arg1) {
    std::string json;
    base::JSONWriter::Write(*arg1, &json);
    javascript.append(json);
    if (arg2) {
      base::JSONWriter::Write(*arg2, &json);
      javascript.append(", ").append(json);
      if (arg3) {
        base::JSONWriter::Write(*arg3, &json);
        javascript.append(", ").append(json);
      }
    }
  }
  javascript.append(");");
  web_contents()->GetMainFrame()->ExecuteJavaScriptForTests(
      base::UTF8ToUTF16(javascript));
}

void WingsWebBindings::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  LOG(INFO) << "unimplemented";
}

void WingsWebBindings::DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                             const std::string& message) {
  // LOG(INFO) << "DispatchProtocolMessage " << message;
  if (message.length() < kMaxMessageChunkSize) {
    std::string param;
    base::EscapeJSONString(message, true, &param);
    std::string code = "DevToolsAPI.dispatchMessage(" + param + ");";
    base::string16 javascript = base::UTF8ToUTF16(code);
    web_contents()->GetMainFrame()->ExecuteJavaScriptForTests(javascript);
    return;
  }

  size_t total_size = message.length();
  for (size_t pos = 0; pos < message.length(); pos += kMaxMessageChunkSize) {
    std::string param;
    base::EscapeJSONString(message.substr(pos, kMaxMessageChunkSize), true,
                           &param);
    std::string code = "DevToolsAPI.dispatchMessageChunk(" + param + "," +
                       std::to_string(pos ? 0 : total_size) + ");";
    base::string16 javascript = base::UTF8ToUTF16(code);
    web_contents()->GetMainFrame()->ExecuteJavaScriptForTests(javascript);
  }
}

void WingsWebBindings::HandleMessageFromDevToolsFrontend(const std::string& message) {
  std::string method;
  base::ListValue* params = nullptr;
  base::DictionaryValue* dict = nullptr;
  std::unique_ptr<base::Value> parsed_message = base::JSONReader::Read(message);
  if (!parsed_message || !parsed_message->GetAsDictionary(&dict) ||
      !dict->GetString("method", &method)) {
    return;
  }
  int request_id = 0;
  dict->GetInteger("id", &request_id);
  dict->GetList("params", &params);

  if (method == "dispatchProtocolMessage" && params && params->GetSize() == 1) {
    std::string protocol_message;
    if (!agent_host_ || !params->GetString(0, &protocol_message))
      return;
    agent_host_->DispatchProtocolMessage(this, protocol_message);
  } else if (method == "loadCompleted") {
    web_contents()->GetMainFrame()->ExecuteJavaScriptForTests(
        base::ASCIIToUTF16("DevToolsAPI.setUseSoftMenu(true);"));
  } else if (method == "getPreferences") {
    SendMessageAck(request_id, &preferences_);
    return;
  } else if (method == "setPreference") {
    std::string name;
    std::string value;
    if (!params->GetString(0, &name) || !params->GetString(1, &value)) {
      return;
    }
    preferences_.SetKey(name, base::Value(value));
  } else if (method == "removePreference") {
    std::string name;
    if (!params->GetString(0, &name))
      return;
    preferences_.RemoveWithoutPathExpansion(name, nullptr);
  } else {
    LOG(INFO) << "unimplemented method: " << method;
    return;
  }

  if (request_id)
    SendMessageAck(request_id, nullptr);  
}

// WebContentsObserver overrides
void WingsWebBindings::ReadyToCommitNavigation(content::NavigationHandle* navigation_handle) {
  content::RenderFrameHost* frame = navigation_handle->GetRenderFrameHost();
  if (navigation_handle->IsInMainFrame()) {
    frontend_host_.reset(WingsWebFrontendHost::Create(
        frame,
        base::Bind(&WingsWebBindings::HandleMessageFromDevToolsFrontend,
                   base::Unretained(this))));
    return;
  }
  std::string origin = navigation_handle->GetURL().GetOrigin().spec();
  auto it = extensions_api_.find(origin);
  if (it == extensions_api_.end())
    return;
  std::string script = base::StringPrintf("%s(\"%s\")", it->second.c_str(),
                                          base::GenerateGUID().c_str());
  content::DevToolsFrontendHost::SetupExtensionsAPI(frame, script);

}

void WingsWebBindings::WebContentsDestroyed() {
  LOG(INFO) << "unimplemented"; 
}

// net::URLFetcherDelegate overrides.
void WingsWebBindings::OnURLFetchComplete(const net::URLFetcher* source) {
  LOG(INFO) << "unimplemented"; 
}

void WingsWebBindings::SendMessageAck(int request_id, const base::Value* arg1) {
  base::Value id_value(request_id);
  CallClientFunction("DevToolsAPI.embedderMessageAck", &id_value, arg1, nullptr);
}

} // namespace wings


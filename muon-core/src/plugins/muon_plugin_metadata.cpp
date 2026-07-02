/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_plugin_metadata.h"

#include <cctype>

static constexpr char kMuonRendererFunctionsKey[] = "muon_functions";
static constexpr char kMuonRendererNamespacesKey[] = "muon_namespaces";
static constexpr char kMuonRendererUrlHintKey[] = "muon_url_hint";
static constexpr char kMuonNamespaceNameKey[] = "namespace";
static constexpr char kMuonNamespaceSetupScriptKey[] = "setup_script";
static constexpr char kMuonNamespaceAllowedFunctionsKey[] =
    "allowed_functions";
static constexpr char kMuonFunctionIdKey[] = "id";
static constexpr char kMuonFunctionNamespaceKey[] = "namespace";
static constexpr char kMuonFunctionNameKey[] = "name";
static constexpr char kMuonFunctionPublicNameKey[] = "public_name";
static constexpr char kMuonFunctionArgumentsKey[] = "args";
static constexpr char kMuonFunctionReturnTypeKey[] = "return_type";
static constexpr char kMuonTypeDescriptorTypeKey[] = "type";
static constexpr char kMuonTypeDescriptorArgsKey[] = "args";
static constexpr char kMuonTypeDescriptorReturnTypeKey[] = "return_type";

bool IsValidMuonJsIdentifier(const std::string& name) {
  if (name.empty()) {
    return false;
  }

  const auto first = static_cast<unsigned char>(name[0]);
  if (!(std::isalpha(first) != 0 || name[0] == '_' || name[0] == '$')) {
    return false;
  }

  for (auto index = size_t{1}; index < name.size(); ++index) {
    const auto character = static_cast<unsigned char>(name[index]);
    if (!(std::isalnum(character) != 0 || name[index] == '_' ||
          name[index] == '$')) {
      return false;
    }
  }
  return true;
}

bool SplitMuonPluginNamespace(const std::string& plugin_namespace,
                               std::vector<std::string>* segments) {
  if (segments != nullptr) {
    segments->clear();
  }
  if (plugin_namespace.empty()) {
    return false;
  }

  auto begin = size_t{0};
  while (begin <= plugin_namespace.size()) {
    const auto dot = plugin_namespace.find('.', begin);
    const auto end = dot == std::string::npos ? plugin_namespace.size() : dot;
    const auto segment = plugin_namespace.substr(begin, end - begin);
    if (!IsValidMuonJsIdentifier(segment)) {
      if (segments != nullptr) {
        segments->clear();
      }
      return false;
    }
    if (segments != nullptr) {
      segments->push_back(segment);
    }
    if (dot == std::string::npos) {
      return true;
    }
    begin = dot + 1;
  }
  if (segments != nullptr) {
    segments->clear();
  }
  return false;
}

bool IsValidMuonPluginNamespace(const std::string& plugin_namespace) {
  return SplitMuonPluginNamespace(plugin_namespace, nullptr);
}

std::string CreateMuonFunctionPublicPath(
    const std::string& plugin_namespace,
    const std::string& js_name) {
  return plugin_namespace + "." + js_name;
}

std::string CreateMuonFunctionPublicPath(
    const MuonFunctionMetadata& function) {
  return CreateMuonFunctionPublicPath(
      function.plugin_namespace,
      function.public_name.empty() ? function.js_name : function.public_name);
}

CefRefPtr<CefDictionaryValue> CreateMuonTypeMetadataDictionary(
    const MuonTypeMetadata& type) {
  auto dictionary = CefDictionaryValue::Create();
  dictionary->SetInt(kMuonTypeDescriptorTypeKey,
                     static_cast<int>(type.type));
  if (type.type == MUON_TYPE_FUNCTION) {
    auto args = CefListValue::Create();
    args->SetSize(type.function_arg_types.size());
    for (auto index = size_t{0}; index < type.function_arg_types.size(); ++index) {
      args->SetDictionary(index, CreateMuonTypeMetadataDictionary(
                                      type.function_arg_types[index]));
    }
    dictionary->SetList(kMuonTypeDescriptorArgsKey, args);
    if (!type.function_return_type.empty()) {
      dictionary->SetDictionary(
          kMuonTypeDescriptorReturnTypeKey,
          CreateMuonTypeMetadataDictionary(type.function_return_type[0]));
    }
  }
  return dictionary;
}

bool ReadMuonTypeMetadataDictionary(CefRefPtr<CefDictionaryValue> dictionary,
                                     bool allow_void,
                                     MuonTypeMetadata* type) {
  if (!dictionary || !dictionary->HasKey(kMuonTypeDescriptorTypeKey)) {
    return false;
  }
  const auto value_type = static_cast<muon_value_type>(
      dictionary->GetInt(kMuonTypeDescriptorTypeKey));
  if (!IsSupportedMuonValueType(value_type) ||
      (!allow_void && value_type == MUON_TYPE_VOID)) {
    return false;
  }

  type->type = value_type;
  type->function_arg_types.clear();
  type->function_return_type.clear();
  if (value_type != MUON_TYPE_FUNCTION) {
    return true;
  }

  const auto args = dictionary->GetList(kMuonTypeDescriptorArgsKey);
  const auto return_dictionary =
      dictionary->GetDictionary(kMuonTypeDescriptorReturnTypeKey);
  if (!args || !return_dictionary) {
    return false;
  }
  type->function_arg_types.resize(args->GetSize());
  for (auto index = size_t{0}; index < args->GetSize(); ++index) {
    if (!ReadMuonTypeMetadataDictionary(args->GetDictionary(index), false,
                                         &type->function_arg_types[index])) {
      return false;
    }
  }
  type->function_return_type.resize(1);
  return ReadMuonTypeMetadataDictionary(return_dictionary, true,
                                         &type->function_return_type[0]);
}

CefRefPtr<CefDictionaryValue> CreateMuonRendererMetadata(
    const std::vector<MuonNamespaceMetadata>& namespaces,
    const std::vector<MuonFunctionMetadata>& functions) {
  auto metadata = CefDictionaryValue::Create();
  auto namespace_list = CefListValue::Create();
  namespace_list->SetSize(namespaces.size());
  for (auto namespace_index = size_t{0}; namespace_index < namespaces.size();
       ++namespace_index) {
    const auto& plugin_namespace = namespaces[namespace_index];
    auto namespace_dictionary = CefDictionaryValue::Create();
    namespace_dictionary->SetString(kMuonNamespaceNameKey,
                                    plugin_namespace.plugin_namespace);
    namespace_dictionary->SetString(kMuonNamespaceSetupScriptKey,
                                    plugin_namespace.setup_script);
    auto allowed_functions = CefListValue::Create();
    allowed_functions->SetSize(plugin_namespace.allowed_function_names.size());
    for (auto function_index = size_t{0};
         function_index < plugin_namespace.allowed_function_names.size();
         ++function_index) {
      allowed_functions->SetString(
          function_index,
          plugin_namespace.allowed_function_names[function_index]);
    }
    namespace_dictionary->SetList(kMuonNamespaceAllowedFunctionsKey,
                                  allowed_functions);
    namespace_list->SetDictionary(namespace_index, namespace_dictionary);
  }
  metadata->SetList(kMuonRendererNamespacesKey, namespace_list);

  auto function_list = CefListValue::Create();
  function_list->SetSize(functions.size());

  for (auto function_index = size_t{0}; function_index < functions.size();
       ++function_index) {
    const auto& function = functions[function_index];
    auto function_dictionary = CefDictionaryValue::Create();
    function_dictionary->SetInt(kMuonFunctionIdKey,
                                static_cast<int>(function.id));
    function_dictionary->SetString(kMuonFunctionNamespaceKey,
                                   function.plugin_namespace);
    function_dictionary->SetString(kMuonFunctionNameKey, function.js_name);
    function_dictionary->SetString(
        kMuonFunctionPublicNameKey,
        function.public_name.empty() ? function.js_name
                                     : function.public_name);
    function_dictionary->SetDictionary(
        kMuonFunctionReturnTypeKey,
        CreateMuonTypeMetadataDictionary(function.return_type));

    auto args = CefListValue::Create();
    args->SetSize(function.arg_types.size());
    for (auto arg_index = size_t{0}; arg_index < function.arg_types.size();
         ++arg_index) {
      args->SetDictionary(arg_index, CreateMuonTypeMetadataDictionary(
                                         function.arg_types[arg_index]));
    }
    function_dictionary->SetList(kMuonFunctionArgumentsKey, args);
    function_list->SetDictionary(function_index, function_dictionary);
  }

  metadata->SetList(kMuonRendererFunctionsKey, function_list);
  return metadata;
}

MuonRendererMetadata ReadMuonRendererMetadata(
    CefRefPtr<CefDictionaryValue> extra_info) {
  MuonRendererMetadata metadata;
  if (!extra_info) {
    return metadata;
  }

  if (extra_info->HasKey(kMuonRendererNamespacesKey)) {
    const auto namespace_list = extra_info->GetList(kMuonRendererNamespacesKey);
    if (namespace_list) {
      for (auto namespace_index = size_t{0};
           namespace_index < namespace_list->GetSize();
           ++namespace_index) {
        const auto namespace_dictionary =
            namespace_list->GetDictionary(namespace_index);
        if (!namespace_dictionary) {
          continue;
        }
        MuonNamespaceMetadata plugin_namespace;
        plugin_namespace.plugin_namespace =
            namespace_dictionary->GetString(kMuonNamespaceNameKey).ToString();
        plugin_namespace.setup_script =
            namespace_dictionary->GetString(
                kMuonNamespaceSetupScriptKey).ToString();
        const auto allowed_functions =
            namespace_dictionary->GetList(kMuonNamespaceAllowedFunctionsKey);
        if (allowed_functions) {
          for (auto function_index = size_t{0};
               function_index < allowed_functions->GetSize();
               ++function_index) {
            const auto function_name =
                allowed_functions->GetString(function_index).ToString();
            if (IsValidMuonJsIdentifier(function_name)) {
              plugin_namespace.allowed_function_names.push_back(function_name);
            }
          }
        }
        if (IsValidMuonPluginNamespace(plugin_namespace.plugin_namespace)) {
          metadata.namespaces.push_back(plugin_namespace);
        }
      }
    }
  }

  if (!extra_info->HasKey(kMuonRendererFunctionsKey)) {
    return metadata;
  }

  const auto function_list = extra_info->GetList(kMuonRendererFunctionsKey);
  if (!function_list) {
    return metadata;
  }

  for (auto function_index = size_t{0}; function_index < function_list->GetSize();
       ++function_index) {
    const auto function_dictionary =
        function_list->GetDictionary(function_index);
    if (!function_dictionary) {
      continue;
    }

    MuonFunctionMetadata function;
    function.id = static_cast<uint32_t>(
        function_dictionary->GetInt(kMuonFunctionIdKey));
    function.plugin_namespace =
        function_dictionary->GetString(kMuonFunctionNamespaceKey).ToString();
    function.js_name =
        function_dictionary->GetString(kMuonFunctionNameKey).ToString();
    function.public_name =
        function_dictionary->HasKey(kMuonFunctionPublicNameKey)
            ? function_dictionary->GetString(
                  kMuonFunctionPublicNameKey).ToString()
            : function.js_name;
    if (!IsValidMuonPluginNamespace(function.plugin_namespace) ||
        !IsValidMuonJsIdentifier(function.js_name) ||
        !IsValidMuonJsIdentifier(function.public_name) ||
        !ReadMuonTypeMetadataDictionary(
            function_dictionary->GetDictionary(kMuonFunctionReturnTypeKey),
            true, &function.return_type)) {
      continue;
    }

    const auto args = function_dictionary->GetList(kMuonFunctionArgumentsKey);
    if (!args) {
      continue;
    }

    auto args_valid = true;
    for (auto arg_index = size_t{0}; arg_index < args->GetSize(); ++arg_index) {
      MuonTypeMetadata type;
      if (!ReadMuonTypeMetadataDictionary(args->GetDictionary(arg_index),
                                           false, &type)) {
        args_valid = false;
        break;
      }
      function.arg_types.push_back(type);
    }
    if (args_valid) {
      metadata.functions.push_back(function);
    }
  }

  return metadata;
}

void WriteMuonRendererUrlHint(CefRefPtr<CefDictionaryValue> extra_info,
                              const std::string& url) {
  if (!extra_info || url.empty()) {
    return;
  }
  extra_info->SetString(kMuonRendererUrlHintKey, url);
}

std::string ReadMuonRendererUrlHint(CefRefPtr<CefDictionaryValue> extra_info) {
  if (!extra_info || !extra_info->HasKey(kMuonRendererUrlHintKey)) {
    return "";
  }
  return extra_info->GetString(kMuonRendererUrlHintKey).ToString();
}

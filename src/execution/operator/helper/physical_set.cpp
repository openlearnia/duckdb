#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace duckdb {

namespace {

//! Session settings changed by SET LOCAL, restored when the transaction ends
class TransactionLocalSettings : public ClientContextState {
public:
	static constexpr const char *KEY = "transaction_local_settings";

	void Record(ClientContext &context, idx_t setting_index) {
		if (previous.find(setting_index) != previous.end()) {
			return;
		}
		auto &user_settings = ClientConfig::GetConfig(context).user_settings;
		PreviousValue entry;
		if (user_settings.IsSet(setting_index)) {
			entry.is_set = true;
			user_settings.TryGetSetting(DBConfig::GetConfig(context).user_settings, setting_index, entry.value);
		}
		previous.emplace(setting_index, std::move(entry));
	}
	void Forget(idx_t setting_index) {
		previous.erase(setting_index);
	}
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		Restore(context);
	}
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		Restore(context);
	}

private:
	struct PreviousValue {
		bool is_set = false;
		Value value;
	};

	void Restore(ClientContext &context) {
		auto &user_settings = ClientConfig::GetConfig(context).user_settings;
		for (auto &entry : previous) {
			if (entry.second.is_set) {
				user_settings.SetUserSetting(entry.first, entry.second.value);
			} else {
				user_settings.ClearSetting(entry.first);
			}
		}
		previous.clear();
	}

	unordered_map<idx_t, PreviousValue> previous;
};

} // namespace

void PhysicalSet::ForgetTransactionLocalSetting(ClientContext &context, idx_t setting_index) {
	auto state = context.registered_state->Get<TransactionLocalSettings>(TransactionLocalSettings::KEY);
	if (state) {
		state->Forget(setting_index);
	}
}

//! SET LOCAL: a session-scope change that only lasts until the current transaction ends
static void SetTransactionLocal(ClientContext &context, idx_t setting_index, Value target_value) {
	if (context.transaction.IsAutoCommit()) {
		throw InvalidInputException("SET LOCAL can only be used in transaction blocks");
	}
	auto state = context.registered_state->GetOrCreate<TransactionLocalSettings>(TransactionLocalSettings::KEY);
	state->Record(context, setting_index);
	Settings::Set(context, setting_index, SetScope::SESSION, std::move(target_value));
}

void PhysicalSet::SetGenericVariable(ClientContext &context, idx_t setting_index, SetScope scope, Value target_value) {
	Settings::Set(context, setting_index, scope, std::move(target_value));
}

void PhysicalSet::SetExtensionVariable(ClientContext &context, ExtensionOption &extension_option, SetScope scope,
                                       const Value &value) {
	auto &target_type = extension_option.type;
	Value target_value = value.CastAs(context, target_type);
	auto setting_index = extension_option.setting_index.GetIndex();
	if (scope == SetScope::LOCAL) {
		SetTransactionLocal(context, setting_index, std::move(target_value));
		return;
	}
	if (extension_option.set_function) {
		extension_option.set_function(context, scope, target_value);
	}
	if (scope == SetScope::AUTOMATIC) {
		scope = extension_option.default_scope;
	}
	if (scope != SetScope::GLOBAL) {
		ForgetTransactionLocalSetting(context, setting_index);
	}
	SetGenericVariable(context, setting_index, scope, std::move(target_value));
}

SetScope PhysicalSet::GetSettingScope(const ConfigurationOption &option, SetScope variable_scope) {
	if (variable_scope == SetScope::AUTOMATIC) {
		if (option.set_local) {
			return SetScope::SESSION;
		}
		if (option.set_global) {
			return SetScope::GLOBAL;
		}
		// generic setting
		switch (option.scope) {
		case SettingScopeTarget::LOCAL_ONLY:
		case SettingScopeTarget::LOCAL_DEFAULT:
			return SetScope::SESSION;
		case SettingScopeTarget::GLOBAL_ONLY:
		case SettingScopeTarget::GLOBAL_DEFAULT:
			return SetScope::GLOBAL;
		default:
			throw InvalidInputException("Setting \"%s\" does not have a valid scope defined", option.name);
		}
	}
	if (variable_scope == SetScope::SESSION && option.scope == SettingScopeTarget::GLOBAL_ONLY) {
		throw InvalidInputException("Setting \"%s\" cannot be set as a session variable - it can only be set globally",
		                            option.name);
	}
	if (variable_scope == SetScope::GLOBAL && option.scope == SettingScopeTarget::LOCAL_ONLY) {
		throw InvalidInputException(
		    "Setting \"%s\" cannot be set as a global variable - it can only be set per session", option.name);
	}
	return variable_scope;
}

void PhysicalSet::SetVariable(ClientContext &context, const Identifier &name, SetScope scope, const Value &value) {
	auto &config = DBConfig::GetConfig(context);
	// check if we are allowed to change the configuration option
	config.CheckLock(name);
	auto option = DBConfig::GetOptionByName(name);
	if (!option) {
		ExtensionOption extension_option;
		// check if this is an extra extension variable
		if (!config.TryGetExtensionOption(name, extension_option)) {
			auto extension_name = Catalog::AutoloadExtensionByConfigName(context, name);
			if (!config.TryGetExtensionOption(name, extension_option)) {
				throw InvalidInputException("Extension parameter %s was not found after autoloading", name);
			}
		}
		if (scope == SetScope::LOCAL && extension_option.set_function) {
			// restoring at transaction end bypasses setting callbacks, so only plain settings qualify
			throw NotImplementedException("SET LOCAL is not supported for setting \"%s\"", name);
		}
		SetExtensionVariable(context, extension_option, scope, value);
		return;
	}
	if (scope == SetScope::LOCAL) {
		// restoring at transaction end bypasses setting callbacks, so only plain session settings qualify
		if (!option->default_value || option->set_callback || option->scope == SettingScopeTarget::GLOBAL_ONLY) {
			throw NotImplementedException("SET LOCAL is not supported for setting \"%s\"", name);
		}
		Value input_val = value.CastAs(context, DBConfig::ParseLogicalType(option->parameter_type));
		SetTransactionLocal(context, option->setting_idx.GetIndex(), std::move(input_val));
		return;
	}
	SetScope variable_scope = GetSettingScope(*option, scope);

	Value input_val = value.CastAs(context, DBConfig::ParseLogicalType(option->parameter_type));
	if (option->default_value) {
		if (option->set_callback) {
			SettingCallbackInfo info(context, variable_scope);
			option->set_callback(info, input_val);
		}
		auto setting_index = option->setting_idx.GetIndex();
		if (variable_scope == SetScope::SESSION) {
			ForgetTransactionLocalSetting(context, setting_index);
		}
		SetGenericVariable(context, setting_index, variable_scope, std::move(input_val));
		return;
	}
	switch (variable_scope) {
	case SetScope::GLOBAL: {
		if (!option->set_global) {
			throw CatalogException("option %s cannot be set globally", name);
		}
		auto &db = DatabaseInstance::GetDatabase(context);
		config.SetOption(&db, *option, input_val);
		break;
	}
	case SetScope::SESSION:
		if (!option->set_local) {
			throw CatalogException("option %s cannot be set locally", name);
		}
		option->set_local(context, input_val);
		break;
	default:
		throw InternalException("Unsupported SetScope for variable");
	}
}

SourceResultType PhysicalSet::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSourceInput &input) const {
	SetVariable(context.client, Identifier(std::string_view(name.data(), name.size())), scope, value);
	return SourceResultType::FINISHED;
}

} // namespace duckdb

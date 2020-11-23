#include "edit.h"
#include "asset/asset-configure-inform.h"
#include "asset/asset-import.h"
#include "asset/asset-manager.h"
#include "asset/csv.h"
#include "asset/logger.h"
#include <cxxtools/jsondeserializer.h>
#include <fty/rest/audit-log.h>
#include <fty/rest/component.h>
#include <fty_asset_activator.h>
#include <fty_common_asset.h>
#include <fty_common_asset_types.h>
#include <fty_common_mlm.h>

namespace fty::asset {

#define AGENT_ASSET_ACTIVATOR "etn-licensing-credits"

unsigned Edit::run()
{
    rest::User user(m_request);
    if (auto ret = checkPermissions(user.profile(), m_permissions); !ret) {
        throw rest::Error(ret.error());
    }

    Expected<std::string> id = m_request.queryArg<std::string>("id");
    if (!id || !persist::is_ok_name(id->c_str())) {
        auditError("Request CREATE OR UPDATE asset FAILED: {}"_tr, "Asset id is not set"_tr);
        throw rest::Error("request-param-bad", "id");
    }

    std::string                 asset_json(m_request.body());
    cxxtools::SerializationInfo si;
    try {
        std::stringstream          input(asset_json, std::ios_base::in);
        cxxtools::JsonDeserializer deserializer(input);
        deserializer.deserialize(si);
        auto        si_id = si.findMember("id");
        std::string status;
        si.getMember("status") >>= status;
        std::string type;
        si.getMember("type") >>= type;

        if (!si_id) {
            si.addMember("id") <<= id;
        } else if (status == "nonactive") {
            if (*id == "rackcontroller-0" || persist::is_container(type)) {
                logDebug("Element {} cannot be inactivated.", *id);
                auditError("Request CREATE OR UPDATE asset id {} FAILED"_tr, *id);
                throw rest::Error("action-forbidden", "Inactivation of this asset"_tr);
            }
        } else {
            si_id->setValue(id);
        }
    } catch (const std::exception& e) {
        auditError("Request CREATE OR UPDATE asset id {} FAILED"_tr, *id);
        throw rest::Error("request-param-bad", e.what());
    }

    try {
        fty::FullAsset asset;
        si >>= asset;

        if (asset.isPowerAsset() && asset.getStatusString() == "active") {
            mlm::MlmSyncClient  client(AGENT_FTY_ASSET, AGENT_ASSET_ACTIVATOR);
            fty::AssetActivator activationAccessor(client);
            if (!activationAccessor.isActivable(asset)) {
                throw std::runtime_error("Asset cannot be activated"_tr);
            }
        }
    } catch (const std::exception& e) {
        auditError("Request CREATE OR UPDATE asset id {} FAILED"_tr, id);
        throw rest::Error("licensing-err", e.what());
    }

    CsvMap cm;
    try {
        cm = CsvMap_from_serialization_info(si);
        cm.setUpdateUser(user.login());
        std::time_t timestamp = std::time(NULL);
        std::string mbstr(100, '\0');
        if (std::strftime(&mbstr[0], sizeof(mbstr), "%FT%T%z", std::localtime(&timestamp))) {
            cm.setUpdateTs(mbstr);
        }
    } catch (const std::invalid_argument& e) {
        logError(e.what());
        auditError("Request CREATE OR UPDATE asset id {} FAILED"_tr, *id);
        throw rest::Error("bad-request-document", e.what());
    } catch (const std::exception& e) {
        std::string err = TRANSLATE_ME("See log for more details");
        auditError("Request CREATE OR UPDATE asset id {} FAILED"_tr, *id);
        throw rest::Error("internal-error", e.what());
    }

    // PUT /asset is currently used to update an existing device (only asset_element and ext_attributes)
    //      for EMC4J.
    // empty document
    if (cm.cols() == 0 || cm.rows() == 0) {
        auditError("Request CREATE OR UPDATE asset id {} FAILED", *id);
        throw rest::Error("bad-request-document", "Cannot import empty document."_tr);
    }

    if (!cm.hasTitle("type")) {
        auditError("Request CREATE OR UPDATE asset id {} FAILED", *id);
        throw rest::Error("request-param-required", "type"_tr);
    }

    logDebug("starting load");
    Import import(cm);
    if (auto res = import.process(true)) {
        const auto& imported = import.items();
        if (imported.find(1) == imported.end()) {
            throw rest::Error("internal-error", "Request CREATE OR UPDATE asset id {} FAILED"_tr.format(*id));
        }

        if (imported.at(1)) {
            // this code can be executed in multiple threads -> agent's name should
            // be unique at the every moment
            std::string agent_name = generateMlmClientId("web.asset_put");
            if (auto sent = sendConfigure(*(imported.at(1)), import.operation(), agent_name); !sent) {
                logError(sent.error());
                throw rest::Error("internal-error", sent.error());
            }

            // no unexpected errors was detected
            // process results
            auto ret = db::idToNameExtName(imported.at(1)->id);
            if (!ret) {
                logError(ret.error());
                throw rest::Error("internal-error", ret.error());
            }
            m_reply << "{\"id\": \"" << ret->first << "\"}";
            auditInfo("Request CREATE OR UPDATE asset id {} SUCCESS"_tr, *id);
            return HTTP_OK;
        } else {
            throw rest::Error("internal-error", "Import failed"_tr);
        }
    } else {
        throw rest::Error("internal-error", res.error());
    }
}

} // namespace fty::asset

registerHandler(fty::asset::Edit)

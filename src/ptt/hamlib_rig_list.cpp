#include "ptt/hamlib_rig_list.hpp"

#include <hamlib/rig.h>

#include <algorithm>
#include <mutex>

namespace ultra::ptt {
namespace {

std::once_flag g_hamlib_init_once;

void initializeHamlibRigList() {
    rig_set_debug(RIG_DEBUG_WARN);
    (void)rig_load_all_backends();
}

int addRig(const struct rig_caps* caps, rig_ptr_t data) {
    if (!caps || !data) {
        return 1;
    }

    auto* rigs = static_cast<std::vector<HamlibRigInfo>*>(data);
    HamlibRigInfo info;
    info.model_id = static_cast<int>(caps->rig_model);
    if (caps->mfg_name) {
        info.vendor = caps->mfg_name;
    }
    if (caps->model_name) {
        info.model_name = caps->model_name;
    }
    if (info.model_id > 0 && !info.model_name.empty()) {
        rigs->push_back(std::move(info));
    }
    return 1;
}

std::string sortKey(const HamlibRigInfo& rig) {
    return rig.vendor + "\n" + rig.model_name + "\n" + std::to_string(rig.model_id);
}

} // namespace

std::string HamlibRigInfo::displayName() const {
    if (vendor.empty()) {
        return model_name;
    }
    if (model_name.empty()) {
        return vendor;
    }
    return vendor + " " + model_name;
}

std::vector<HamlibRigInfo> enumerateHamlibRigs() {
    std::call_once(g_hamlib_init_once, initializeHamlibRigList);

    std::vector<HamlibRigInfo> rigs;
    rig_list_foreach(addRig, &rigs);
    std::sort(rigs.begin(), rigs.end(), [](const HamlibRigInfo& lhs,
                                           const HamlibRigInfo& rhs) {
        return sortKey(lhs) < sortKey(rhs);
    });

    rigs.erase(std::unique(rigs.begin(), rigs.end(),
                           [](const HamlibRigInfo& lhs, const HamlibRigInfo& rhs) {
                               return lhs.model_id == rhs.model_id;
                           }),
               rigs.end());
    return rigs;
}

const std::vector<HamlibRigInfo>& cachedHamlibRigList() {
    static const std::vector<HamlibRigInfo> rigs = enumerateHamlibRigs();
    return rigs;
}

const HamlibRigInfo* findHamlibRigByModelId(int model_id) {
    const auto& rigs = cachedHamlibRigList();
    const auto it = std::find_if(rigs.begin(), rigs.end(), [model_id](const HamlibRigInfo& rig) {
        return rig.model_id == model_id;
    });
    return it == rigs.end() ? nullptr : &*it;
}

} // namespace ultra::ptt

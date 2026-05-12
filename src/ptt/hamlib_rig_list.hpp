#pragma once

#include <string>
#include <vector>

namespace ultra::ptt {

struct HamlibRigInfo {
    int model_id = 0;
    std::string vendor;
    std::string model_name;

    std::string displayName() const;
};

std::vector<HamlibRigInfo> enumerateHamlibRigs();
const std::vector<HamlibRigInfo>& cachedHamlibRigList();
const HamlibRigInfo* findHamlibRigByModelId(int model_id);

} // namespace ultra::ptt

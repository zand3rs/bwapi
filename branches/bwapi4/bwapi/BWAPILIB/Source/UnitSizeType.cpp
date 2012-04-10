#include <string>
#include <map>
#include <BWAPI/UnitSizeType.h>
#include <Util/Foreach.h>

#include "Common.h"

#include "../../Debug.h"

namespace BWAPI
{
  static const std::string unitSizeTypeName[UnitSizeTypes::Enum::MAX] =
  {
    "Independent",
    "Small",
    "Medium",
    "Large",
    "None",
    "Unknown"  
  };
  std::map<std::string, UnitSizeType> unitSizeTypeMap;
  namespace UnitSizeTypes
  {
    BWAPI_TYPEDEF(UnitSizeType,Independent);
    BWAPI_TYPEDEF(UnitSizeType,Small);
    BWAPI_TYPEDEF(UnitSizeType,Medium);
    BWAPI_TYPEDEF(UnitSizeType,Large);
    BWAPI_TYPEDEF(UnitSizeType,None);
    BWAPI_TYPEDEF(UnitSizeType,Unknown);

    static const UnitSizeType::set unitSizeTypeSet( Independent | Small | Medium | Large | None | Unknown );
    void init()
    {
      foreach(UnitSizeType i, unitSizeTypeSet)
      {
        std::string name(i.getName());
        fixName(&name);
        unitSizeTypeMap.insert(std::make_pair(name, i));
      }
    }
  }
  UnitSizeType::UnitSizeType(int id) : Type( id )
  {
  }
  const std::string &UnitSizeType::getName() const
  {
    return unitSizeTypeName[this->getID()];
  }
  const char *UnitSizeType::c_str() const
  {
    return unitSizeTypeName[this->getID()].c_str();
  }
  UnitSizeType UnitSizeTypes::getUnitSizeType(std::string name)
  {
    fixName(&name);
    std::map<std::string, UnitSizeType>::iterator i = unitSizeTypeMap.find(name);
    if (i == unitSizeTypeMap.end())
      return UnitSizeTypes::Unknown;
    return (*i).second;
  }
  const UnitSizeType::set& UnitSizeTypes::allUnitSizeTypes()
  {
    return UnitSizeTypes::unitSizeTypeSet;
  }
}


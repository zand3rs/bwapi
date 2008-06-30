#pragma once

#include <list>

#include "Command.h"

#include <BW/UnitType.h>

namespace BuildOrder { class BuildWeights; }
class TiXmlElement;

namespace BuildOrder
{ 
 /** Represents command in buld-order to change factory(producing building) priorities. */
  class CommandChangeWeights : public Command
  {
     public :
      CommandChangeWeights(TiXmlElement* xmlElement);
      virtual ~CommandChangeWeights();
      virtual bool execute();
    private :
      BuildWeights* weights;
  };
}
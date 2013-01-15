#include "GameImpl.h"
#include <Util/Foreach.h>
#include <cmath>
#include <ctime>

#include "../Detours.h"
#include "../DLLMain.h"
#include "../Config.h"
#include "../Recording.h"

#include <BW/Pathing.h>
#include <BW/CUnit.h>
#include <BW/Dialog.h>

#include <BWAPI/PlayerImpl.h>
#include <BWAPI/RegionImpl.h>
#include <BWAPI/Command.h>
#include <BWAPI/Map.h>

#include "../../../svnrev.h"
#include "../../../Debug.h"

#define TOURNAMENT_STR "BWAPI r" SVN_REV_STR " Tournament Mode Engaged!"

namespace BWAPI
{
  //--------------------------------------------- ON GAME START ----------------------------------------------
  void GameImpl::onGameStart()
  {
    // This function is called at the start of every match
    this->initializeData();

    // initialize the variables
    //this->frameCount      = 0;
    this->startTickCount  = GetTickCount();
    this->onStartCalled   = true;
    this->calledMatchEnd  = false;

    // load the map data
    map.load();
    this->savedMapHash = Map::getMapHash();

    // Obtain Broodwar Regions
    if ( *BW::BWDATA::SAIPathing )
    {
      u32 rgnCount = (*BW::BWDATA::SAIPathing)->regionCount;
      // Iterate regions and insert into region list
      for ( u32 i = 0; i < rgnCount; ++i )
        this->regionsList.insert(new BWAPI::RegionImpl(i));

      // Iterate regions again and update neighbor lists
      foreach ( BWAPI::RegionImpl *r, this->regionsList )
        r->UpdateRegionRelations();
    } // if SAI_Pathing

    // roughly identify which players can possibly participate in this game
    // iterate triggers for each player
    for ( int i = 0; i < PLAYABLE_PLAYER_COUNT; ++i )
    {
      // reset participation and resources
      if ( this->players[i] )
      {
        this->players[i]->setParticipating(false);
        this->players[i]->resetResources();
      }

      // First check if player owns a unit at start
      for ( int u = 0; u < UnitTypes::None; ++u )
      {
        if ( BW::BWDATA::AllScores->unitCounts.all[u][i] )
        {
          if ( this->players[i] )
            this->players[i]->setParticipating();
          break;
        }
      }

      // Then iterate each trigger
      // checking if a unit can be created or given to the player later in the game
      for ( BW::BlizzVectorEntry<BW::Triggers::Trigger> *t = BW::BWDATA::TriggerVectors[i].begin; (u32)t != ~(u32)&BW::BWDATA::TriggerVectors[i].end && (u32)t != (u32)&BW::BWDATA::TriggerVectors[i].begin; t = t->next )
      {
        // check if trigger conditions can be met
        if ( t->container.conditionsCanBeMet() )
        {
          // check participation of players
          for ( int p = 0; p < PLAYABLE_PLAYER_COUNT; ++p )
          {
            // Don't bother checking for participation if the player doesn't exist
            // or if the player is already participating (NOT observing)
            if ( !this->players[p] || !this->players[p]->isObserver() )
              continue;
            // Check if trigger actions allow gameplay and set participation if it does.
            if ( t->container.actionsAllowGameplay(i, p) )
              this->players[p]->setParticipating();
          }
        } // conds can be met
      } // trigvector iterator
    } // playercount iterator


    if ( *(BW::BWDATA::InReplay) ) /* set replay flags */
    {
      // Set every cheat flag to true
      for (int i = 0; i < Flag::Max; ++i)
        this->flags[i] = true;
    }
    else
    {
      /* Get the current player */
      BWAPI::PlayerImpl *thisPlayer = this->_getPlayer(_currentPlayerId());
      if ( !thisPlayer )
        return;

      this->BWAPIPlayer = thisPlayer;
      /* find the opponent player */
      for ( int i = 0; i < PLAYABLE_PLAYER_COUNT; ++i )
      {
        if ( (this->players[i]->getType() == PlayerTypes::Computer ||
              this->players[i]->getType() == PlayerTypes::Player   ||
              this->players[i]->getType() == PlayerTypes::EitherPreferComputer) &&
             !this->players[i]->isObserver() &&
             thisPlayer->isEnemy(this->players[i]) )
          this->enemyPlayer = this->players[i];
      }
    }
    
    // get pre-race info
    BYTE bRaceInfo[12] = { 0 };
    BYTE bOwnerInfo[12] = { 0 };

    HANDLE hFile = NULL;
    if ( SFileOpenFileEx(nullptr, "staredit\\scenario.chk", SFILE_FROM_MPQ, &hFile) && hFile )
    {
      DWORD dwFilesize = SFileGetFileSize(hFile, nullptr);
      void *pData = SMAlloc(dwFilesize);
      if ( pData )
      {
        DWORD dwRead = 0;
        if ( SFileReadFile(hFile, pData, dwFilesize, &dwRead, 0) && dwRead == dwFilesize )
        {
          struct _mapchunk
          {
            DWORD dwId;
            DWORD dwSize;
            BYTE  bData[1];
          } *mcptr;
          for ( mcptr = (_mapchunk*)pData; (DWORD)mcptr < (DWORD)pData + dwFilesize; mcptr = (_mapchunk*)&mcptr->bData[mcptr->dwSize] )
          {
            switch ( mcptr->dwId )
            {
            case MAKEFOURCC('S', 'I', 'D', 'E'):
              if ( mcptr->dwSize == 12 )
                memcpy(bRaceInfo, mcptr->bData, 12);
              break;
            case MAKEFOURCC('O', 'W', 'N', 'R'):
              if ( mcptr->dwSize == 12 )
                memcpy(bOwnerInfo, mcptr->bData, 12);
              break;
            }
          }
        }
        SMFree(pData);
      }
      SFileCloseFile(hFile);
    }

    // get the set of start locations
    BW::Position *StartLocs = BW::BWDATA::startPositions;
    // Iterate all players
    for ( int i = 0; i < PLAYABLE_PLAYER_COUNT; ++i )
    {
      // Skip Start Locations that don't exist
      if ( StartLocs[i] == Position(0, 0) )
        continue;

      // If the game is UMS and player is observer and race is not (UserSelect OR invalid player type), skip
      if ( this->getGameType() == GameTypes::Use_Map_Settings && 
           this->players[i]->isObserver() && 
           (bRaceInfo[i] != Races::Enum::Select ||
           (bOwnerInfo[i] != PlayerTypes::Computer &&
            bOwnerInfo[i] != PlayerTypes::Player   &&
            bOwnerInfo[i] != PlayerTypes::EitherPreferComputer &&
            bOwnerInfo[i] != PlayerTypes::EitherPreferHuman)) )
        continue;

      // add start location
      startLocations.push_back( TilePosition(StartLocs[i] - Position(64, 48)) );
    }

    // Get Player Objects
    for ( int i = 0; i < PLAYABLE_PLAYER_COUNT; ++i )
    {
      if ( this->players[i] && 
           BW::BWDATA::Players[i].nType != PlayerTypes::None &&
           BW::BWDATA::Players[i].nType <  PlayerTypes::Closed )
      {
        players[i]->setID(server.getPlayerID(players[i]));
        this->playerSet.insert(this->players[i]);
      }
    }

    if ( this->BWAPIPlayer )
    {
      foreach(Player* p, players)
      {
        if ( p->leftGame() || p->isDefeated() || p == BWAPIPlayer )
          continue;
        if ( BWAPIPlayer->isAlly(p) )
          _allies.insert(p);
        if ( BWAPIPlayer->isEnemy(p) )
          _enemies.insert(p);
        if ( p->isObserver() )
          _observers.insert(p);
      }
    }

    if ( this->players[11] )
      this->playerSet.insert(this->players[11]);

    // Get Force Objects, assign Force<->Player relations
    ForceImpl *pNeutralForce = new ForceImpl(std::string(""));
    pNeutralForce->players.insert(this->players[11]);
    this->players[11]->force = pNeutralForce;

    for ( int f = 1; f <= 4; ++f )
    {
      ForceImpl *newForce = new ForceImpl( std::string(BW::BWDATA::ForceNames[f-1].name) );
      this->forces.insert( newForce );
      for ( int p = 0; p < PLAYABLE_PLAYER_COUNT; ++p )
      {
        if ( this->players[p] && BW::BWDATA::Players[p].nTeam == f )
        {
          this->players[p]->force = newForce;
          if ( BW::BWDATA::Players[p].nType != PlayerTypes::None &&
               BW::BWDATA::Players[p].nType <  PlayerTypes::Closed )
            newForce->players.insert(this->players[p]);
        }
      }
    }

    // Assign neutral force to players that do not have one
    for ( int p = 0; p < PLAYABLE_PLAYER_COUNT; ++p )
    {
      if ( this->players[p] && !this->players[p]->force )
        this->players[p]->force = pNeutralForce;
    }

    // Get info for replay naming
    if ( !this->isReplay() )
    {
      if ( BWAPIPlayer )
      {
        rn_BWAPIName = BWAPIPlayer->getName();
        rn_BWAPIRace = BWAPIPlayer->getRace().getName().substr(0, 1);
      }
      rn_MapName   = mapName().substr(0, 16);
      rn_AlliesNames.clear();
      rn_AlliesRaces.clear();
      rn_EnemiesNames.clear();
      rn_EnemiesRaces.clear();
      foreach ( Player *p, this->_allies )
      {
        if ( p )
        {
          rn_AlliesNames += p->getName().substr(0, 6);
          rn_AlliesRaces += p->getRace().getName().substr(0, 1);
        }
      }
      foreach ( Player *p, this->_enemies )
      {
        if ( p )
        {
          rn_EnemiesNames += p->getName().substr(0, 6);
          rn_EnemiesRaces += p->getRace().getName().substr(0, 1);
        }
      }
    } // !isReplay
  }
  //------------------------------------------------- UPDATE -------------------------------------------------
  void GameImpl::update()
  {
    //this function is called every frame from a hook attached in DllMain.cpp
    this->inGame = true;

    // Compute frame rate
    accumulatedFrames++;
    DWORD currentTickCount = GetTickCount();
    if ( currentTickCount >= lastTickCount + 1000 )
    {
      fps               = accumulatedFrames;
      averageFPS        = averageFPS*0.7+fps*0.3;

      double APMInterval = 0.95;
      int duration = (currentTickCount - lastTickCount);
      int totalDuration = (currentTickCount - startTickCount);
      botAPMCounter_selects     = botAPMCounter_selects * exp(-(duration)/(APMInterval*60000));
      botAPMCounter_noselects   = botAPMCounter_noselects * exp(-(duration)/(APMInterval*60000));
      double gameDurationFactor = 1-exp(-totalDuration/(APMInterval*60000));
      if (gameDurationFactor < 0.01) gameDurationFactor = 0.01; //Prevent division by 0
      botAPM_selects   = (int)floor(botAPMCounter_noselects+botAPMCounter_selects/(APMInterval*gameDurationFactor));
      botAPM_noselects = (int)floor(botAPMCounter_noselects/(APMInterval*gameDurationFactor));

      lastTickCount     = currentTickCount;
      accumulatedFrames = 0;
    }

    try
    {
      //the first time update() is called, we also call onGameStart to initialize some things
      if ( !onStartCalled )
        this->onGameStart();
      
      if ( !this->calledMatchEnd && frameCount > 1 )
      {
        bool win     = false;
        bool allDone = false;
        if ( this->BWAPIPlayer )
        {
          if ( this->BWAPIPlayer->isVictorious() )
          {
            win     = true;
            allDone = true;
          }
          if ( this->BWAPIPlayer->isDefeated() )
          {
            win     = false;
            allDone = true;
          }
            
        }
        else
        {
          allDone = true;
          foreach(PlayerImpl *p, this->players)
          {
            if ( p->getIndex() >= 8 )
              continue;
            if ( !p->isDefeated() && !p->isVictorious() && !p->leftGame() )
              allDone = false;
          }
        }
        if ( allDone)
        {
          this->calledMatchEnd = true;
          events.push_back(Event::MatchFrame());
          events.push_back(Event::MatchEnd(win));
          server.update();
          this->inGame = false;
          events.push_back(Event::MenuFrame());
          server.update();
        }
      }

      //don't have any more MatchFrame events after MatchEnd until MatchStart is called.
      if ( this->calledMatchEnd ) return;

      // Update unit selection
      if ( wantSelectionUpdate && memcmp(savedUnitSelection, BW::BWDATA::ClientSelectionGroup, sizeof(savedUnitSelection)) != 0 )
      {
        wantSelectionUpdate = false;
        memcpy(savedUnitSelection, BW::BWDATA::ClientSelectionGroup, sizeof(savedUnitSelection));
        refreshSelectionStates();
      }

      //update players and check to see if they have just left the game.
      _allies.clear();
      _enemies.clear();
      _observers.clear();
      if ( BWAPIPlayer )
      {
        foreach(Player* p, players)
        {
          if ( p->leftGame() || p->isDefeated() || p == BWAPIPlayer )
            continue;
          if ( BWAPIPlayer->isAlly(p) )
            _allies.insert(p);
          if ( BWAPIPlayer->isEnemy(p) )
            _enemies.insert(p);
          if ( p->isObserver() )
            _observers.insert(p);
        }
      }
      for (int i = 0; i < PLAYER_COUNT; ++i)
      {
        bool prevLeftGame = this->players[i]->leftGame();
        this->players[i]->updateData();
        if (!prevLeftGame && this->players[i]->leftGame())
          events.push_back(Event::PlayerLeft((Player*)this->players[i]));
      }
      //update properties of Unit and Bullet objects
      this->updateUnits();
      this->updateBullets();

      //iterate through the list of intercepted messages
      foreach(std::string i, sentMessages)
        BroodwarImpl.onSendText(i.c_str());

      //clear all intercepted messages
      this->sentMessages.clear();
    }
    catch (GeneralException& exception)
    {
      BWAPIError("Exception caught inside Game::update: %s", exception.getMessage().c_str());
    }

    //on the first frame we check to see if the client process has connected.
    //if not, then we load the AI dll specified in bwapi.ini
    if ( !this->startedClient )
    {
      // Declare typedefs for function pointers
      typedef void (*PFNGameInit)(Game *);
      typedef AIModule* (*PFNCreateA1)();
      typedef TournamentModule* (*PFNCreateTournament)();

      // Initialize Tournament Variables
#ifndef _DEBUG
      // Load tournament string and module if string exists
      std::string TournamentDllPath = LoadConfigString("ai", "tournament");
      if ( TournamentDllPath.size() > 0 )
        hTournamentModule = LoadLibrary(TournamentDllPath.c_str());

      // If tournament module exists
      if ( hTournamentModule )
      {
        // Obtain our tournament functions
        PFNGameInit         newGameInit         = (PFNGameInit)GetProcAddress(hTournamentModule, TEXT("gameInit"));
        PFNCreateA1         newTournamentAI     = (PFNCreateA1)GetProcAddress(hTournamentModule, TEXT("newTournamentAI"));
        PFNCreateTournament newTournamentModule = (PFNCreateTournament)GetProcAddress(hTournamentModule, TEXT("newTournamentModule"));

        // Call the tournament functions if they exist
        if ( newTournamentAI && newTournamentModule && newGameInit )
        {
          newGameInit(this);
          this->tournamentAI         = newTournamentAI();
          this->tournamentController = newTournamentModule();
        }
        else // error when one function is not found
        {
          // Free the tournament module
          FreeLibrary(hTournamentModule);
          hTournamentModule = NULL;

          // Create our error string
          std::string missing;
          if ( !newTournamentAI )
            missing += "newTournamentAI";
          
          if ( !newTournamentModule )
          {
            if ( !missing.empty() )
              missing += " and ";
            missing += "newTournamentModule";
          }
          missing += " function";

          // print error message
          Broodwar << Text::Red << "ERROR: Failed to find the " << missing << " in tournament module." << std::endl;
        }
      }
      this->bTournamentMessageAppeared = false;
#else
      this->bTournamentMessageAppeared = true;
#endif

      // Connect to external module if it exists
      externalModuleConnected = false;
      std::string moduleName("<Nothing>");
      if ( server.isConnected() ) //check to see if the server is connected to the client
      {
        // assign a blank AI module to our variable
        this->client = new AIModule();
        // Hide success strings in tournament mode
        if ( !hTournamentModule )
          Broodwar << "BWAPI: Connected to AI Client process" << std::endl;
        // Set the module string
        moduleName = "<Client Connection>";
        externalModuleConnected = true;
      }
      else // if not, load the AI module DLL
      {
        // declare/assign variables
        hAIModule         = nullptr;

        std::string aicfg = LoadConfigString("ai", BUILD_DEBUG ? "ai_dbg" : "ai", "_NULL");
        std::stringstream aiList(aicfg);

        // Get DLL name
        std::string dll = aicfg.substr(0, aicfg.find_first_of(','));

        // Skip to current intended instance
        for ( int i = 0; i < (int)gdwProcNum && aiList; ++i )
          std::getline(aiList, dll, ',');

        // ignore the deprecated revision extension
        size_t tmp = dll.find_first_of(':');
        if ( tmp != std::string::npos )
          dll.erase(tmp);

        // Trim leading and trailing spaces
        while ( isspace(dll.front()) )
          dll.erase(0);
        while ( isspace(dll.back())  )
          dll.pop_back();

        // Check if string was loaded
        if ( aicfg == "_NULL" )
          BWAPIError("Could not find %s under ai in \"%s\".", BUILD_DEBUG ? "ai_dbg" : "ai", configPath.c_str());
        else  // Load DLL
          hAIModule = LoadLibrary(dll.c_str());

        if ( !hAIModule )
        {
          //if hAIModule is a NULL pointer, there there was a problem when trying to load the AI Module
          this->client = new AIModule();

          // enable flags to allow interaction
          Broodwar->enableFlag(Flag::CompleteMapInformation);
          Broodwar->enableFlag(Flag::UserInput);

          // print error string
          Broodwar << Text::Red << "ERROR: Failed to load the AI Module \"" << dll << "\"." << std::endl;
          externalModuleConnected = false;
        }
        else
        {
          // Obtain the AI module function
          PFNGameInit newGame     = (PFNGameInit)GetProcAddress(hAIModule, TEXT("gameInit"));
          PFNCreateA1 newAIModule = (PFNCreateA1)GetProcAddress(hAIModule, TEXT("newAIModule"));
          if ( newAIModule && newGame )
          {
            // Call the AI module function and assign the client variable
            newGame(this);
            this->client = newAIModule();

            // Hide success strings in tournament mode
            if ( !hTournamentModule )
              Broodwar << Text::Green << "Loaded the AI Module: " << dll << std::endl;
            externalModuleConnected = true;

            moduleName = dll;

            // Strip the path from the module name
            size_t tmp = moduleName.find_last_of("/\\");
            if ( tmp != std::string::npos )
              moduleName.erase(0, tmp+1);
          }
          else  // If the AIModule function is not found
          {
            // Create a dummy AI module
            this->client = new AIModule();

            // Enable flags to allow interaction
            Broodwar->enableFlag(Flag::CompleteMapInformation);
            Broodwar->enableFlag(Flag::UserInput);

            // Create error string
            std::string missing;
            if ( !newGame )
              missing += "gameInit";
          
            if ( !newAIModule )
            {
              if ( !missing.empty() )
                missing += " and ";
              missing += "newAIModule";
            }
            missing += " function";

            // Print an error message
            Broodwar << Text::Red << "ERROR: Failed to find the " << missing << " in " << dll << std::endl;
            externalModuleConnected = false;
          }
        }
      }

      //push the MatchStart event to the front of the queue so that it is the first event in the queue.
      events.push_front(Event::MatchStart());
      this->startedClient = true;
      // Don't send text in tournament mode
      if ( !hTournamentModule )
        sendText("BWAPI r" SVN_REV_STR " " BUILD_STR " is now live using \"%s\".", moduleName.c_str() );
    }

    if ( !this->bTournamentMessageAppeared && 
          hTournamentModule && 
          this->frameCount > _currentPlayerId()*8 )
    {
      this->bTournamentMessageAppeared = true;
      this->isTournamentCall = true;
      sendText("%s", TOURNAMENT_STR);
      if ( this->tournamentController )
        this->tournamentController->onFirstAdvertisement();
      this->isTournamentCall = false;
    }

    //each frame we add a MatchFrame event to the queue
    events.push_back(Event::MatchFrame());

    //if the AI is a client process, this will signal the client to process the next frame
    //if the AI is a DLL, this will translate the events into AIModule callbacks.
    server.update();

    //Before returning control to starcraft, we clear the unit data for units that are no longer accessible
    foreach(UnitImpl* u, evadeUnits)
      u->updateData();

    //We also kill the units that are dying on this frame.
    //We wait until after server.update() and processEvents() to do this so that the AI can
    //access the last frame of unit data during the onUnitDestroy callback.
    foreach(UnitImpl* u, dyingUnits)
    {
      deadUnits.push_back(u);
      int index = u->getIndex();
      unitArray[index] = new UnitImpl(&BW::BWDATA::UnitNodeTable[index],(u16)index);
      u->die();
    }

    /* In case we ever want to add a Flag::UnitPermanence cheat flag...
    bool UnitPermanence = false;
    if ( !UnitPermanence )
    {
      foreach(UnitImpl* u, evadeUnits)
      {
        deadUnits.push_back(u);
        int index = u->getIndex();
        unitArray[index] = new UnitImpl(&BW::BWDATA::UnitNodeTable->unit[index],(u16)index);
        u->die();
      }
    }
    */

    //increment frame count if the game is not paused
    if ( *BW::BWDATA::isGamePaused == 0 )
      this->frameCount++;

    // Set the replay time, this is a workaround to fixing the replay DIVIDE_BY_ZERO exception bug
    if ( !this->isReplay() )
      *BW::BWDATA::ReplayFrames = this->getFrameCount()+20;

    // Check if the window is iconic, if so, go super fast!
    static bool bLastIconic = false;
    if ( !!IsIconic(SDrawGetFrameWindow()) != bLastIconic && !this->isMultiplayer() )
    {
      this->setLocalSpeed(bLastIconic ? -1 : 0);
      this->setFrameSkip(bLastIconic ? 1 : 16);
      bLastIconic = !bLastIconic;
    }

    // If we should process our commands just before sending them
    // @TODO: Only process on the frame before commands are sent
    //if ( *BW::BWDATA::FramesUntilNextTurn == 1 )
    {
      // Iterate the command types
      for ( int i = 0; i < UnitCommandTypes::Enum::None; ++i )
      {
        // Declare our temporary variables
        Unitset groupOf12;

        int e = 0, x = 0, y = 0;
        Unit      *t = nullptr;
        UnitType  ut;
        bool      o = false;

        // Iterate the list
        auto c = commandOptimizer[i].begin();

        // Re-Iterate all remaining commands
        while ( c != commandOptimizer[i].end() )
        {
          // Iterate all commands, and only process those that are equal
          while ( c != commandOptimizer[i].end() )
          {
            // Ignore anything but the command that the unit last processed
            if ( //static_cast<UnitImpl*>(c->unit)->lastImmediateCommandFrame == this->getFrameCount() &&
                   static_cast<UnitImpl*>(c->unit)->lastImmediateCommand != *c )
            {
              c = commandOptimizer[i].erase(c);
              continue;
            }

            // If we are starting a new command grouping
            if ( groupOf12.empty() )
            {
              // Assign our comparison variables to determine which commands should be grouped
              // Note: Using individual variables instead of comparing UnitCommand operator== because
              //       it will also compare the type which is not necessary, and we may create a new
              //       optimization type that does a fuzzy position comparison
              e = c->extra;
              t = c->target;
              x = c->x;
              y = c->y;
              if (  i == UnitCommandTypes::Attack_Unit ||
                    i == UnitCommandTypes::Unload_All  ||
                    i == UnitCommandTypes::Load        ||
                    i == UnitCommandTypes::Cancel_Morph )
                o = c->unit->getType().isBuilding();
              else if ( i == UnitCommandTypes::Use_Tech )
                o = c->unit->isSieged() || c->unit->isCloaked() || c->unit->isBurrowed();
              else
                o = false;
              groupOf12.push_back( c->unit );
              c = commandOptimizer[i].erase(c);
            } // otherwise if this command is the same as the first, the units can be grouped
            else if ( e == c->extra && t == c->target && x == c->x && y == c->y )
            {
              bool oTmp;
              if (  i == UnitCommandTypes::Attack_Unit ||
                    i == UnitCommandTypes::Unload_All  ||
                    i == UnitCommandTypes::Load        ||
                    i == UnitCommandTypes::Cancel_Morph )
                oTmp = c->unit->getType().isBuilding();
              else if ( i == UnitCommandTypes::Use_Tech )
                oTmp = c->unit->isSieged() || c->unit->isCloaked() || c->unit->isBurrowed();
              else
                oTmp = false;

              if ( o == oTmp )
              {
                groupOf12.push_back( c->unit );
                c = commandOptimizer[i].erase(c);
              }
              else
                ++c;
            } // otherwise skip this command for now
            else
            {
              ++c;
            }

            // If our group of 12 is full
            if ( groupOf12.size() == 12 )
            {
              // Select the group
              BW::Orders::Select sel(groupOf12);
              ++botAPM_select;
              QueueGameCommand(&sel, sel.size);

              // Workaround for doing certain actions
              Unit *unit = (i == UnitCommandTypes::Load         || 
                            i == UnitCommandTypes::Attack_Unit  ||
                            i == UnitCommandTypes::Train        ||
                            i == UnitCommandTypes::Unload_All   ||
                            i == UnitCommandTypes::Cancel_Morph ||
                            i == UnitCommandTypes::Use_Tech) ? groupOf12.front() : nullptr;

              // execute command
              executeCommand(UnitCommand(unit, i, t, x, y, e));
              groupOf12.clear();
            } // groupOf12 max execute
          } // second while

          // If we iterated the entire command list and don't have an empty group, then give a command
          // to the remaining units in the group
          if ( !groupOf12.empty() )
          {
            // Select the group
            BW::Orders::Select sel(groupOf12);
            ++botAPM_select;
            QueueGameCommand(&sel, sel.size);

            // Workaround for doing certain actions
            Unit *unit = (i == UnitCommandTypes::Load         || 
                          i == UnitCommandTypes::Attack_Unit  ||
                          i == UnitCommandTypes::Train        ||
                          i == UnitCommandTypes::Unload_All   ||
                          i == UnitCommandTypes::Cancel_Morph ||
                          i == UnitCommandTypes::Use_Tech) ? groupOf12.front() : nullptr;

            // execute command
            executeCommand(UnitCommand(unit, i, t, x, y, e));
            groupOf12.clear();
          }
          // Reset iterator
          c = commandOptimizer[i].begin();
        } // first while
      } // iterate command types
    } // execute all stored commands

    // grid
    if ( grid )
    {
      BWAPI::Position scrPos = getScreenPosition();

      // draw mtx grid
      for ( int y = scrPos.y/32; y < (scrPos.y + BW::BWDATA::GameScreenBuffer->height())/32 + 1; ++y )
      {
        for ( int x = scrPos.x/32; x < (scrPos.x + BW::BWDATA::GameScreenBuffer->width())/32 + 1; ++x )
        {
          for ( int i = 0; i < 32; i += 4 )
          {
            drawLineMap(x*32 + 32, y*32 + i, x*32 + 32, y*32 + i + 2, BWAPI::Colors::Grey);
            drawLineMap(x*32 + i, y*32 + 32, x*32 + i + 2, y*32 + 32, BWAPI::Colors::Grey);
          }
        }
      }
      setTextSize(0);
      drawTextScreen(64, 288, "%c(%u, %u)", Text::White, (scrPos.x+this->getMousePosition().x)/32, (scrPos.y+this->getMousePosition().y)/32);
      setTextSize();
    } // grid

    if ( showfps )
    {
      setTextSize(0);
      drawTextScreen(1,1, "%cFrame Rate (Logical Frames)\nLFPS: %d\nAvg LFPS:%.3lf", BWAPI::Text::Yellow, this->getFPS(), this->getAverageFPS());
      setTextSize();
    } // fps

#ifdef _DEBUG
    ////////////////////////////////////////////// Ignore rest if GUI disabled
    if ( !data->hasGUI )
      return;

    setTextSize(0);
    // unitdebug
    if ( unitDebug && BWAPIPlayer )
    {
      foreach ( BWAPI::UnitImpl *u, this->getAllUnits() )
      {
        auto raw = u->getOriginalRawData;
        if ( raw->autoTargetUnit )
          this->drawLineMap(u->getPosition(), BWAPI::Position(raw->autoTargetUnit->position), Colors::Red);
      }
      /*
      BWAPI::Color c = Colors::Red;
      int x = this->getMousePosition().x + this->getScreenPosition().x;
      int y = this->getMousePosition().y + this->getScreenPosition().y;
      if ( BW::isCollidingWithContour((*BW::BWDATA::SAIPathing)->contours, 
                                  x,
                                  y,
                                  UnitTypes::Terran_Marine.dimensionLeft(),
                                  UnitTypes::Terran_Marine.dimensionUp(),
                                  UnitTypes::Terran_Marine.dimensionRight(),
                                  UnitTypes::Terran_Marine.dimensionDown()) )
         c = Colors::Green;
      drawBoxMap(x - UnitTypes::Terran_Marine.dimensionLeft(),
                                  y - UnitTypes::Terran_Marine.dimensionUp(),
                                  x + UnitTypes::Terran_Marine.dimensionRight(),
                                  y + UnitTypes::Terran_Marine.dimensionDown(), c);
      */
    } // unitdebug

    // pathdebug
    if ( pathDebug && *BW::BWDATA::SAIPathing )
    {
      BWAPI::Position mouse   = getMousePosition() + getScreenPosition();
      BW::region *selectedRgn = BW::getRegionAt(mouse);
      int scrx = (getScreenPosition().x/32 - 1)*32;
      int scry = (getScreenPosition().y/32 - 1)*32;
      for ( int x = (scrx > 0 ? scrx : 0); x < getScreenPosition().x + BW::BWDATA::ScreenLayers[5].width && x/32 < this->mapWidth(); x += 32 )
      {
        for ( int y = (scry > 0 ? scry : 0); y < getScreenPosition().y + BW::BWDATA::ScreenLayers[5].height && y/32 < this->mapHeight(); y += 32 )
        {
          BW::TilePosition tp((u16)x/32, (u16)y/32);

          u16 idx = (*BW::BWDATA::SAIPathing)->mapTileRegionId[tp.y][tp.x];
          if ( idx & 0x2000 )
          {
            BW::split *t = &(*BW::BWDATA::SAIPathing)->splitTiles[idx & 0x1FFF];
            for ( int mTileY = 0; mTileY < 4; ++mTileY )
            {
              for ( int mTileX = 0; mTileX < 4; ++mTileX )
              {
                BW::region *rgn1 = getRegionFromId(t->rgn1);
                BWAPI::Color c = selectedRgn == rgn1 ? BWAPI::Colors::Brown : BWAPI::Colors::Grey;
                if ( rgn1->accessabilityFlags == 0x1FFD )
                  c = selectedRgn == rgn1 ? BWAPI::Colors::Yellow : BWAPI::Colors::Red;
                if ( ((t->minitileMask >> ( mTileX + mTileY * 4 )) & 1) )
                {
                  BW::region *rgn2 = getRegionFromId(t->rgn2);
                  c = selectedRgn == rgn2 ? BWAPI::Colors::Brown : BWAPI::Colors::Grey;
                  if ( rgn2->accessabilityFlags == 0x1FFD )
                    c = selectedRgn == rgn2 ? BWAPI::Colors::Yellow : BWAPI::Colors::Red;
                }
                drawLineMap(x + mTileX * 8,     y + mTileY * 8, x + mTileX * 8 + 8, y + mTileY * 8 + 8, c);
                drawLineMap(x + mTileX * 8 + 8, y + mTileY * 8, x + mTileX * 8,     y + mTileY * 8 + 8, c);
              } // minitile X
            }// minitile Y
          } // index & 0x2000
          else
          {
            BW::region *r = getRegionFromId(idx);
            BWAPI::Color c = selectedRgn == r ? BWAPI::Colors::Brown : BWAPI::Colors::Grey;
            if ( r->accessabilityFlags == 0x1FFD )
              c = selectedRgn == r ? BWAPI::Colors::Yellow : BWAPI::Colors::Red;
            drawLineMap(x,    y,    x + 32,      y + 32,      c);
            drawLineMap(x+8,  y,    x + 32,      y + 32 - 8,  c);
            drawLineMap(x+16, y,    x + 32,      y + 32 - 16, c);
            drawLineMap(x+24, y,    x + 32,      y + 32 - 24, c);
            drawLineMap(x,    y+16, x + 32 - 16, y + 32,      c);
            drawLineMap(x,    y+8,  x + 32 - 8,  y + 32,      c);
            drawLineMap(x,    y+24, x + 32 - 24, y + 32,      c);
            
            drawLineMap(x,    y+32, x + 32,      y,           c);
            drawLineMap(x,    y+16, x + 32 - 16, y,           c);
            drawLineMap(x,    y+8,  x + 32 - 24, y,           c);
            drawLineMap(x,    y+24, x + 32 - 8,  y,           c);
            drawLineMap(x+16, y+32, x + 32,      y + 32 - 16, c);
            drawLineMap(x+8,  y+32, x + 32,      y + 32 - 24, c);
            drawLineMap(x+24, y+32, x + 32,      y + 32 - 8,  c);
          }
        } // iterate y
      } // iterate x
      for ( unsigned int i = 0; i < (*BW::BWDATA::SAIPathing)->regionCount; ++i )
      {
        BW::region *r = &(*BW::BWDATA::SAIPathing)->regions[i];
        if ( r->accessabilityFlags != 0x1FFD )
          drawBoxMap(r->rgnBox.left, r->rgnBox.top, r->rgnBox.right, r->rgnBox.bottom, r == selectedRgn ? Colors::Cyan : Colors::Purple);

        for ( u8 n = 0; n < r->neighborCount; ++n )
        {
          BW::region *neighbor = r->getNeighbor(n);
          if ( r->accessabilityFlags != 0x1FFD && neighbor->accessabilityFlags != 0x1FFD )
            drawLineMap(r->getCenter(), neighbor->getCenter(), neighbor->groupIndex == r->groupIndex ? Colors::Green : Colors::Red);
        }
        if ( r == selectedRgn )
          drawTextMap(r->getCenter(), "%cTiles: %u\nPaths: %u\nFlags: %u\nGroupID: %u", Text::White, r->tileCount, r->pathCount, r->defencePriority, r->groupIndex);
      }
      for ( int i = 0; i < 4; ++i )
      {
        BW::contourHub *hub = (*BW::BWDATA::SAIPathing)->contours;
        for ( int c = 0; c < hub->contourCount[i]; ++c )
        {
          BW::contour *cont = &hub->contours[i][c];
          bool select = false;
          int l = getScreenPosition().x;
          int r = getScreenPosition().x + BW::BWDATA::ScreenLayers[5].width;
          int t = getScreenPosition().y;
          int b = getScreenPosition().y + BW::BWDATA::ScreenLayers[5].height;

          switch ( cont->type )
          {
          case 0:
          case 2:
            if ( !((cont->v[1] > l || cont->v[2] > l) && (cont->v[1] < r || cont->v[2] < r) && cont->v[0] > t && cont->v[0] < b) )
              continue;
            if ( mouse.x < cont->v[2] && mouse.x > cont->v[1] && mouse.y > cont->v[0] - 4 && mouse.y < cont->v[0] + 4 )
              select = true;
            drawLineMap(cont->v[1], cont->v[0], cont->v[2], cont->v[0], select ? Colors::Cyan : Colors::Green);
            break;
          case 1:
          case 3:
            if ( !((cont->v[1] > t || cont->v[2] > t) && (cont->v[1] < b || cont->v[2] < b) && cont->v[0] > l && cont->v[0] < r) )
              continue;
            if ( mouse.x < cont->v[0] + 4 && mouse.x > cont->v[0] - 4 && mouse.y > cont->v[1] && mouse.y < cont->v[2] )
              select = true;
            drawLineMap(cont->v[0], cont->v[1], cont->v[0], cont->v[2], select ? Colors::Cyan : Colors::Green);
            break;
          default:
            Broodwar << "Default case! " << cont->type << std::endl;
            break;
          }
          if ( select )
          {
            BYTE b = cont->unk_relation;
            unsigned int len = abs(cont->v[2] - cont->v[1]);
            drawTextMouse(32, 0, "%cLength: %u\n%cUnknown: %u (0x%02X) (b%u%u%u%u%u%u%u%u)", Text::White, len, Text::White, b, b, b&0x80 ? 1:0, b&0x40 ? 1:0, b&0x20 ? 1:0, b&0x10 ? 1:0, b&8 ? 1:0, b&4 ? 1:0, b&2 ? 1:0, b&1 ? 1:0);
          }
        }
      }
      /*
      foreach (BWAPI::RegionImpl *r, this->regionsList )
      {
        drawTextMap(r->getCenter(), "%u", r->getRegionGroupID());
        
        std::vector<BWAPI::Position> poly = r->getSimplePolygon();
        BWAPI::Position prev = Positions::None;
        for ( auto j = poly.begin(), jend = poly.end(); j != jend; ++j )
        {
          if ( prev != Positions::None )
            drawLineMap(prev, *j, Colors::Yellow);
          prev = *j;
        }
      }*/
    } // pathdebug
#endif
    if ( !this->isPaused()  && 
          recordingStarted  && 
          pVidBuffer        && 
          recordingUpdated )
    {
      recordingUpdated = false;
      RecordFrame(wmode ? pVidBuffer : BW::BWDATA::GameScreenBuffer->getData());
    }
    setTextSize(); // Reset text size

    //finally return control to starcraft
  }
  //--------------------------------------------------- ON SAVE ------------------------------------------------
  void GameImpl::onSaveGame(char *name)
  {
    /* called when the game is being saved */
    events.push_back(Event::SaveGame(name));
  }
  //---------------------------------------------- ON SEND TEXT ----------------------------------------------
  void GameImpl::onSendText(const char* text)
  {
    if ( !text )
      return;

    if ( !parseText(text) && isFlagEnabled(BWAPI::Flag::UserInput) )
    {
      if ( externalModuleConnected )
      {
        events.push_back(Event::SendText());
        events.back().setText(text);
      }
      else
        sendText("%s", text);
    }
  }
  //---------------------------------------------- ON RECV TEXT ----------------------------------------------
  void GameImpl::onReceiveText(int playerId, std::string text)
  {
    if ( !this->bTournamentMessageAppeared && hTournamentModule && text == TOURNAMENT_STR )
      this->bTournamentMessageAppeared = true;

    /* Do onReceiveText */
    int realId = stormIdToPlayerId(playerId);
    if ( realId != -1 && 
         (!this->BWAPIPlayer ||
          realId != this->BWAPIPlayer->getIndex() ) &&
         this->isFlagEnabled(BWAPI::Flag::UserInput) )
    {
      events.push_back(Event::ReceiveText(this->players[realId]));
      events.back().setText(text.c_str());
    }
  }
  int fixPathString(const char *in, char *out, size_t outLen)
  {
    unsigned int n = 0;
    const unsigned char *_in = (const unsigned char*)in;
    for ( unsigned int i = 0; _in[i] != 0 && n < outLen-1; ++i )
    {
      if ( !iscntrl(_in[i]) &&
           _in[i] != '?'    &&
           _in[i] != '*'    &&
           _in[i] != '<'    &&
           _in[i] != '|'    &&
           _in[i] != '"'    &&
           _in[i] != ':' )
      {
        if ( _in[i] == '/' )
          out[n] = '\\';
        else
          out[n] = _in[i];
        ++n;
      }
    }
    out[n] = 0;
    return n;
  }

  void ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
  {}
  //---------------------------------------------- ON GAME END -----------------------------------------------
  void GameImpl::onGameEnd()
  {
    //this is called at the end of every match
    if ( !this->onStartCalled )
      return;

    if ( autoMenuSaveReplay != "" && !this->isReplay() )
    {
      // Set replay envvars
      SetEnvironmentVariable("BOTNAME",    rn_BWAPIName.c_str());
      SetEnvironmentVariable("BOTNAME6",   rn_BWAPIName.substr(0,6).c_str());
      SetEnvironmentVariable("BOTRACE",    rn_BWAPIRace.c_str());
      SetEnvironmentVariable("MAP",        rn_MapName.c_str());
      SetEnvironmentVariable("ALLYNAMES",  rn_AlliesNames.c_str());
      SetEnvironmentVariable("ALLYRACES",  rn_AlliesRaces.c_str());
      SetEnvironmentVariable("ENEMYNAMES", rn_EnemiesNames.c_str());
      SetEnvironmentVariable("ENEMYRACES", rn_EnemiesRaces.c_str());

      // Expand environment strings to szInterPath
      char szTmpPath[MAX_PATH] = { 0 };
      ExpandEnvironmentStrings(autoMenuSaveReplay.c_str(), szTmpPath, MAX_PATH);

      std::string pathStr(szTmpPath);

      // Double any %'s remaining in the string so that strftime executes correctly
      size_t tmp = std::string::npos;
      while ( tmp = pathStr.find_last_of('%', tmp-1), tmp != std::string::npos )
        pathStr.insert(tmp, "%");

      // Replace the placeholder $'s with %'s for the strftime call
      std::replace(pathStr.begin(), pathStr.end(), '$', '%');

      // Get time
      time_t tmpTime = time(nullptr);
      tm *timeInfo = localtime(&tmpTime);
      
      // Expand time strings
      _invalid_parameter_handler old = _set_invalid_parameter_handler(&ignore_invalid_parameter);
        strftime(szTmpPath, sizeof(szTmpPath), pathStr.c_str(), timeInfo);
      _set_invalid_parameter_handler(old);
      pathStr = szTmpPath;

      // Remove illegal characters
      pathStr.erase(std::remove_if(pathStr.begin(), pathStr.end(), [](char c){ return iscntrl(c) ||  c == '?' || c == '*' ||
                                                                                c == '<' ||  c == '|' || c == '>' || c == '"' ||
                                                                                c == ':';}), pathStr.end());
      // Create the directory tree
      size_t pos = 0;
      while ( pos = pathStr.find_first_of("/\\", pos+1), pos != std::string::npos )
        CreateDirectory(pathStr.substr(0,pos).c_str(), nullptr);

      // Copy to global desired replay name
      gDesiredReplayName = pathStr;
    }

    if ( !this->calledMatchEnd )
    {
      this->calledMatchEnd = true;
      events.push_back(Event::MatchFrame());
      events.push_back(Event::MatchEnd(false));
      server.update();
      this->inGame = false;
      events.push_back(Event::MenuFrame());
      server.update();
    }

    // player-specific game end
    for (int i = 0 ; i < PLAYER_COUNT; ++i)
      if ( this->players[i] )
        this->players[i]->onGameEnd();

    this->endTick       = GetTickCount();
    this->onStartCalled = false;

    this->initializeData();
  }
  //---------------------------------------------- SEND EVENTS TO CLIENT
  void GameImpl::SendClientEvent(BWAPI::AIModule *module, Event &e)
  {
    EventType::Enum et = e.getType();
    switch (et)
    {
    case EventType::MatchStart:
      module->onStart();
      break;
    case EventType::MatchEnd:
      module->onEnd(e.isWinner());
      break;
    case EventType::MatchFrame:
      module->onFrame();
      break;
    case EventType::MenuFrame:
      break;
    case EventType::SendText:
      module->onSendText(e.getText());
      break;
    case EventType::ReceiveText:
      module->onReceiveText(e.getPlayer(), e.getText());
      break;
    case EventType::PlayerLeft:
      module->onPlayerLeft(e.getPlayer());
      break;
    case EventType::NukeDetect:
      module->onNukeDetect(e.getPosition());
      break;
    case EventType::UnitDiscover:
      module->onUnitDiscover(e.getUnit());
      break;
    case EventType::UnitEvade:
      module->onUnitEvade(e.getUnit());
      break;
    case EventType::UnitCreate:
      module->onUnitCreate(e.getUnit());
      break;
    case EventType::UnitDestroy:
      module->onUnitDestroy(e.getUnit());
      break;
    case EventType::UnitMorph:
      module->onUnitMorph(e.getUnit());
      break;
    case EventType::UnitShow:
      module->onUnitShow(e.getUnit());
      break;
    case EventType::UnitHide:
      module->onUnitHide(e.getUnit());
      break;
    case EventType::UnitRenegade:
      module->onUnitRenegade(e.getUnit());
      break;
    case EventType::SaveGame:
      module->onSaveGame(e.getText());
      break;
    case EventType::UnitComplete:
      module->onUnitComplete(e.getUnit());
      break;
    default:
      break;
    }
  }
  //---------------------------------------------- PROCESS EVENTS
  void GameImpl::processEvents()
  {
    //This function translates events into AIModule callbacks
    if ( !client || server.isConnected() )
      return;
    foreach(Event e, events)
    {
      static DWORD dwLastEventTime = 0;

      // Reset event stopwatch
      if ( tournamentAI )
      {
        this->lastEventTime = 0;
        dwLastEventTime     = GetTickCount();
      }

      // Send event to the AI Client module
      SendClientEvent(client, e);

      // continue if the tournament is not loaded
      if ( !tournamentAI )
        continue;

      // Save the last event time
      this->lastEventTime = GetTickCount() - dwLastEventTime;

      // Send same event to the Tournament module for post-processing
      isTournamentCall = true;
      SendClientEvent(tournamentAI, e);
      isTournamentCall = false;
    } // foreach event
  }
}


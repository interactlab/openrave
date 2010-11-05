
// Copyright (C) 2006-2010 Carnegie Mellon University (rdiankov@cs.cmu.edu)
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "libopenrave.h"

#include <boost/scoped_ptr.hpp>
#include <boost/utility.hpp>
#include <boost/thread/once.hpp>

#include <streambuf>
#include "mt19937ar.h"
#include "md5.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <locale>

#include "plugindatabase.h"

namespace OpenRAVE {

#ifdef _WIN32
const char s_filesep = '\\';
#else
const char s_filesep = '/';
#endif

static boost::once_flag _onceRaveInitialize = BOOST_ONCE_INIT;

/// there is only once global openrave state. It is created when openrave
/// is first used, and destroyed when the program quits or RaveDestroy is called.
class RaveGlobal : private boost::noncopyable, public boost::enable_shared_from_this<RaveGlobal>
{
    typedef std::map<std::string, CreateXMLReaderFn, CaseInsensitiveCompare> READERSMAP;

    RaveGlobal()
    {
        // is this really necessary?
        //srand(timeGetTime());
        //RaveInitRandomGeneration(timeGetTime());
        _nDebugLevel = Level_Info;

        _mapinterfacenames[PT_Planner] = "planner";
        _mapinterfacenames[PT_Robot] = "robot";
        _mapinterfacenames[PT_SensorSystem] = "sensorsystem";
        _mapinterfacenames[PT_Controller] = "controller";
        _mapinterfacenames[PT_ProblemInstance] = "probleminstance";
        _mapinterfacenames[PT_InverseKinematicsSolver] = "inversekinematicssolver";
        _mapinterfacenames[PT_KinBody] = "kinbody";
        _mapinterfacenames[PT_PhysicsEngine] = "physicsengine";
        _mapinterfacenames[PT_Sensor] = "sensor";
        _mapinterfacenames[PT_CollisionChecker] = "collisionchecker";
        _mapinterfacenames[PT_Trajectory] = "trajectory";
        _mapinterfacenames[PT_Viewer] = "viewer";
        BOOST_ASSERT(_mapinterfacenames.size()==PT_NumberOfInterfaces);
    }
public:
    virtual ~RaveGlobal() {
        Destroy();
    }
    
    static boost::shared_ptr<RaveGlobal>& instance()
    {
        boost::call_once(_create,_onceRaveInitialize);
        return _state;
    }
    
    int Initialize(bool bLoadAllPlugins, DebugLevel level)
    {
        if( IsInitialized() ) {
            return 0; // already initialized
        }

        // set to the classic locale so that number serialization/hashing works correctly
        std::locale::global(std::locale::classic());
        _nDebugLevel = level;

        _pdatabase.reset(new RaveDatabase());
        if( bLoadAllPlugins ) {
            vector<std::string> vplugindirs;
            RaveParseDirectories(getenv("OPENRAVE_PLUGINS"), vplugindirs);
            bool bExists=false;
#ifdef HAVE_BOOST_FILESYSTEM
            boost::filesystem::path pluginsfilename = boost::filesystem::system_complete(boost::filesystem::path(OPENRAVE_PLUGINS_INSTALL_DIR, boost::filesystem::native));
            FOREACH(itname, vplugindirs) {
                if( pluginsfilename == boost::filesystem::system_complete(boost::filesystem::path(*itname, boost::filesystem::native)) ) {
                    bExists = true;
                    break;
                }
            }
#else
            string pluginsfilename=OPENRAVE_PLUGINS_INSTALL_DIR;
            FOREACH(itname, vplugindirs) {
                if( pluginsfilename == *itname ) {
                    bExists = true;
                    break;
                }
            }
#endif
            if( !bExists ) {
                vplugindirs.push_back(OPENRAVE_PLUGINS_INSTALL_DIR);
            }
            FOREACH(it, vplugindirs) {
                if( it->size() > 0 ) {
                    _pdatabase->AddDirectory(it->c_str());
                }
            }
        }

        char* phomedir = getenv("OPENRAVE_HOME");
        if( phomedir == NULL ) {
#ifndef _WIN32
            _homedirectory = string(getenv("HOME"))+string("/.openrave");
#else
            _homedirectory = string(getenv("HOMEDRIVE"))+string(getenv("HOMEPATH"))+string("\\.openrave");
#endif
        }
        else {
            _homedirectory = phomedir;
        }
#ifndef _WIN32
        mkdir(_homedirectory.c_str(),S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IRWXU);
#else
        CreateDirectory(_homedirectory.c_str(),NULL);
#endif

        _vdbdirectories.clear();
        if( !RaveParseDirectories(getenv("OPENRAVE_DATABASE"), _vdbdirectories) ) {
            _vdbdirectories.push_back(_homedirectory);
        }

        return 0;
    }

    void Destroy()
    {
        RAVELOG_VERBOSE("shutting down openrave\n");
        // environments have to be destroyed carefully since their destructors can be called, which will attempt to unregister the environment
        std::list<EnvironmentBase*> listenvironments;
        {
            boost::mutex::scoped_lock lock(_mutexXML);
            listenvironments.swap(_listenvironments);
        }
        FOREACH(itenv,listenvironments) {
            // equire a shared pointer to prevent environment from getting deleted during Destroy loop
            EnvironmentBasePtr penv = (*itenv)->shared_from_this();
            penv->Destroy();
        }
        listenvironments.clear();

        _mapreaders.clear();
        _pdatabase.reset();
    }

    std::string GetHomeDirectory()
    {
        return _homedirectory;
    }

    std::string FindDatabaseFile(const std::string& filename, bool bRead)
    {
        FOREACH(itdirectory,_vdbdirectories) {
#ifdef HAVE_BOOST_FILESYSTEM
            std::string fullfilename = boost::filesystem::system_complete(boost::filesystem::path(*itdirectory, boost::filesystem::native)/filename).string();
#else
            std::string fullfilename = *itdirectory;
            fullfilename += s_filesep;
            fullfilename += filename;
#endif
            if( bRead ) {
                if( !!ifstream(fullfilename.c_str()) ) {
                    return fullfilename;
                }
            }
            else {
                return fullfilename;
            }
            //#endif
        }
        return "";
    }

    void SetDebugLevel(DebugLevel level)
    {
        _nDebugLevel = level;
    }

    DebugLevel GetDebugLevel()
    {
        return _nDebugLevel;
    }

    boost::shared_ptr<void> RegisterXMLReader(InterfaceType type, const std::string& xmltag, const CreateXMLReaderFn& fn)
    {
        boost::mutex::scoped_lock lock(_mutexXML);
        CreateXMLReaderFn oldfn = _mapreaders[type][xmltag];
        _mapreaders[type][xmltag] = fn;
        return boost::shared_ptr<void>((void*)1, boost::bind(&RaveGlobal::_UnregisterXMLReader,boost::weak_ptr<RaveGlobal>(shared_from_this()),type,xmltag,oldfn));
    }

    const BaseXMLReaderPtr CallXMLReader(InterfaceType type, const std::string& xmltag, InterfaceBasePtr pinterface, const std::list<std::pair<std::string,std::string> >& atts)
    {
        READERSMAP::iterator it = _mapreaders[type].find(xmltag);
        if( it == _mapreaders[type].end() ) {
            //throw openrave_exception(str(boost::format("No function registered for interface %s xml tag %s")%GetInterfaceName(type)%xmltag),ORE_InvalidArguments);
            return BaseXMLReaderPtr();
        }
        return it->second(pinterface,atts);
    }

    boost::shared_ptr<RaveDatabase> GetDatabase() const { return _pdatabase; }
    const std::map<InterfaceType,std::string>& GetInterfaceNamesMap() const { return _mapinterfacenames; }

    const std::string& GetInterfaceName(InterfaceType type)
    {
        std::map<InterfaceType,std::string>::const_iterator it = _mapinterfacenames.find(type);
        if( it == _mapinterfacenames.end() ) {
            throw openrave_exception(str(boost::format("Invalid type %d specified")%type));
        }
        return it->second;
    }

    // have to take in pointer instead of shared_ptr since method will be called in EnvironmentBase constructor
    void RegisterEnvironment(EnvironmentBase* penv)
    {
        BOOST_ASSERT(!!_pdatabase);
        boost::mutex::scoped_lock lock(_mutexXML);
        _listenvironments.push_back(penv);
    }

    void UnregisterEnvironment(EnvironmentBase* penv)
    {
        boost::mutex::scoped_lock lock(_mutexXML);
        _listenvironments.remove(penv);
    }

    void GetEnvironments(std::list<EnvironmentBasePtr>& listenvironments)
    {
        listenvironments.clear();
        boost::mutex::scoped_lock lock(_mutexXML);
        FOREACH(it,_listenvironments) {
            EnvironmentBasePtr penv = (*it)->shared_from_this();
            if( !!penv ) {
                listenvironments.push_back(penv);
            }
        }
    }

protected:
    static void _UnregisterXMLReader(boost::weak_ptr<RaveGlobal> pweakstate, InterfaceType type, const std::string& xmltag, const CreateXMLReaderFn& oldfn)
    {
        boost::shared_ptr<RaveGlobal> pstate = pweakstate.lock();
        if( !!pstate ) {
            boost::mutex::scoped_lock lock(pstate->_mutexXML);
            pstate->_mapreaders[type][xmltag] = oldfn;
        }
    }

    static void _create()
    {
        if( !_state ) {
            _state.reset(new RaveGlobal());
        }
    }

    bool IsInitialized() const { return !!_pdatabase; }

private:
    static boost::shared_ptr<RaveGlobal> _state;
    // state that is always present

    // state that is initialized/destroyed
    boost::shared_ptr<RaveDatabase> _pdatabase;
    DebugLevel _nDebugLevel;
    boost::mutex _mutexXML;
    std::map<InterfaceType, READERSMAP > _mapreaders;
    std::map<InterfaceType,string> _mapinterfacenames;
    std::list<EnvironmentBase*> _listenvironments;
    std::string _homedirectory;
    std::vector<std::string> _vdbdirectories;

    friend void RaveInitializeFromState(boost::shared_ptr<void>);
    friend boost::shared_ptr<void> RaveGlobalState();
};

boost::shared_ptr<RaveGlobal> RaveGlobal::_state;

RAVE_API void RaveSetDebugLevel(DebugLevel level)
{
    RaveGlobal::instance()->SetDebugLevel(level);
}

RAVE_API DebugLevel RaveGetDebugLevel()
{
    return RaveGlobal::instance()->GetDebugLevel();
}

RAVE_API const std::map<InterfaceType,std::string>& RaveGetInterfaceNamesMap()
{
    return RaveGlobal::instance()->GetInterfaceNamesMap();
}

RAVE_API const std::string& RaveGetInterfaceName(InterfaceType type)
{
    return RaveGlobal::instance()->GetInterfaceName(type);
}

std::string RaveGetHomeDirectory()
{
    return RaveGlobal::instance()->GetHomeDirectory();
}

std::string RaveFindDatabaseFile(const std::string& filename, bool bRead)
{
    return RaveGlobal::instance()->FindDatabaseFile(filename,bRead);
}

int RaveInitialize(bool bLoadAllPlugins, DebugLevel level)
{
    return RaveGlobal::instance()->Initialize(bLoadAllPlugins,level);
}

void RaveInitializeFromState(boost::shared_ptr<void> globalstate)
{
    RaveGlobal::_state = boost::static_pointer_cast<RaveGlobal>(globalstate);
}

boost::shared_ptr<void> RaveGlobalState()
{
    // only return valid pointer if initialized!
    boost::shared_ptr<RaveGlobal> state = RaveGlobal::_state;
    if( !!state && state->IsInitialized() ) {
        return state;
    }
    return boost::shared_ptr<void>();
}
   
void RaveDestroy()
{
    RaveGlobal::instance()->Destroy();
}

void RaveGetEnvironments(std::list<EnvironmentBasePtr>& listenvironments)
{
    RaveGlobal::instance()->GetEnvironments(listenvironments);
}

void RaveGetPluginInfo(std::list< std::pair<std::string, PLUGININFO> >& plugins)
{
    RaveGlobal::instance()->GetDatabase()->GetPluginInfo(plugins);
}

void RaveGetLoadedInterfaces(std::map<InterfaceType, std::vector<std::string> >& interfacenames)
{
    RaveGlobal::instance()->GetDatabase()->GetLoadedInterfaces(interfacenames);
}

void RaveReloadPlugins()
{
    RaveGlobal::instance()->GetDatabase()->ReloadPlugins();
}

bool RaveLoadPlugin(const std::string& libraryname)
{
    return RaveGlobal::instance()->GetDatabase()->LoadPlugin(libraryname);
}

bool RaveHasInterface(InterfaceType type, const std::string& interfacename)
{
    return RaveGlobal::instance()->GetDatabase()->HasInterface(type,interfacename);
}

InterfaceBasePtr RaveCreateInterface(EnvironmentBasePtr penv, InterfaceType type,const std::string& interfacename)
{
    return RaveGlobal::instance()->GetDatabase()->Create(penv, type,interfacename);
}

RobotBasePtr RaveCreateRobot(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateRobot(penv,name);
}

PlannerBasePtr RaveCreatePlanner(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreatePlanner(penv, name);
}

SensorSystemBasePtr RaveCreateSensorSystem(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateSensorSystem(penv, name);
}

ControllerBasePtr RaveCreateController(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateController(penv, name);
}

ProblemInstancePtr RaveCreateProblem(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateProblem(penv, name);
}

IkSolverBasePtr RaveCreateIkSolver(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateIkSolver(penv, name);
}

PhysicsEngineBasePtr RaveCreatePhysicsEngine(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreatePhysicsEngine(penv, name);
}

SensorBasePtr RaveCreateSensor(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateSensor(penv, name);
}

CollisionCheckerBasePtr RaveCreateCollisionChecker(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateCollisionChecker(penv, name);
}

ViewerBasePtr RaveCreateViewer(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateViewer(penv, name);
}

KinBodyPtr RaveCreateKinBody(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateKinBody(penv, name);
}

TrajectoryBasePtr RaveCreateTrajectory(EnvironmentBasePtr penv, int nDOF)
{
    TrajectoryBasePtr ptraj = RaveGlobal::instance()->GetDatabase()->CreateTrajectory(penv,"");
    if( !!ptraj ) {
        ptraj->Reset(nDOF);
    }
    return ptraj;
}

TrajectoryBasePtr RaveCreateTrajectory(EnvironmentBasePtr penv, const std::string& name)
{
    return RaveGlobal::instance()->GetDatabase()->CreateTrajectory(penv, name);
}

boost::shared_ptr<void> RaveRegisterXMLReader(InterfaceType type, const std::string& xmltag, const CreateXMLReaderFn& fn)
{
    return RaveGlobal::instance()->RegisterXMLReader(type,xmltag,fn);
}


BaseXMLReaderPtr RaveCallXMLReader(InterfaceType type, const std::string& xmltag, InterfaceBasePtr pinterface, const std::list<std::pair<std::string,std::string> >& atts)
{
    return RaveGlobal::instance()->CallXMLReader(type,xmltag,pinterface,atts);
}

void CollisionReport::Reset(int coloptions)
{
    options = coloptions;
    minDistance = 1e20f;
    numCols = 0;
    numWithinTol = 0;
    contacts.resize(0);
    vLinkColliding.resize(0);
}

std::string CollisionReport::__str__() const
{
    stringstream s;
    s << "(";
    if( !!plink1 )
        s << plink1->GetParent()->GetName() << ":" << plink1->GetName();
    s << ")x(";
    if( !!plink2 )
        s << plink2->GetParent()->GetName() << ":" << plink2->GetName();
    s << ") contacts="<<contacts.size();
    return s.str();
}

// Dummy Reader
DummyXMLReader::DummyXMLReader(const std::string& fieldname, const std::string& pparentname, boost::shared_ptr<std::ostream> osrecord) : _fieldname(fieldname), _osrecord(osrecord)
{
    _parentname = pparentname;
    _parentname += ":";
    _parentname += _fieldname;
}

BaseXMLReader::ProcessElement DummyXMLReader::startElement(const std::string& name, const std::list<std::pair<std::string,std::string> >& atts)
{
    if( !!_pcurreader ) {
        if( _pcurreader->startElement(name, atts) == PE_Support )
            return PE_Support;
        return PE_Ignore;
    }

    if( !!_osrecord ) {
        *_osrecord << "<" << name << " ";
        FOREACHC(itatt, atts)
            *_osrecord << itatt->first << "=\"" << itatt->second << "\" ";
        *_osrecord << ">" << endl;
    }
    
    // create a new parser
    _pcurreader.reset(new DummyXMLReader(name, _parentname,_osrecord));
    return PE_Support;
}
    
bool DummyXMLReader::endElement(const std::string& name)
{
    if( !!_pcurreader ) {
        if( _pcurreader->endElement(name) ) {
            _pcurreader.reset();
            if( !!_osrecord )
                *_osrecord << "</" << name << ">" << endl;
        }
        return false;
    }

    if( name == _fieldname )
        return true;
    RAVELOG_ERRORA(str(boost::format("invalid xml tag %s\n")%name));
    return false;
}

void DummyXMLReader::characters(const std::string& ch)
{
    if( !_pcurreader ) {
        if( !!_osrecord )
            *_osrecord << ch;
    }
    else {
        _pcurreader->characters(ch);
    }
}

void subtractstates(std::vector<dReal>& q1, const std::vector<dReal>& q2)
{
    BOOST_ASSERT(q1.size()==q2.size());
    for(size_t i = 0; i < q1.size(); ++i)
        q1[i] -= q2[i];
}

// PlannerParameters class
PlannerBase::PlannerParameters::PlannerParameters() : XMLReadable("plannerparameters"), _fStepLength(0.04f), _nMaxIterations(0), _sPathOptimizationPlanner("shortcut_linear"), _bCheckSelfCollisions(true)
{
    _diffstatefn = subtractstates;
    _vXMLParameters.reserve(10);
    _vXMLParameters.push_back("_vinitialconfig");
    _vXMLParameters.push_back("_vgoalconfig");
    _vXMLParameters.push_back("_vconfiglowerlimit");
    _vXMLParameters.push_back("_vconfigupperlimit");
    _vXMLParameters.push_back("_vconfigresolution");
    _vXMLParameters.push_back("_tworkspacegoal");
    _vXMLParameters.push_back("_nmaxiterations");
    _vXMLParameters.push_back("_fsteplength");
    _vXMLParameters.push_back("_pathoptimization");
    _vXMLParameters.push_back("_bcheckselfcollisions");
}

PlannerBase::PlannerParameters& PlannerBase::PlannerParameters::operator=(const PlannerBase::PlannerParameters& r)
{
    // reset
    _costfn = r._costfn;
    _goalfn = r._goalfn;
    _distmetricfn = r._distmetricfn;
    _constraintfn = r._constraintfn;
    _samplefn = r._samplefn;
    _sampleneighfn = r._sampleneighfn;
    _samplegoalfn = r._samplegoalfn;
    _setstatefn = r._setstatefn;
    _getstatefn = r._getstatefn;
    _diffstatefn = r._diffstatefn;

    _tWorkspaceGoal.reset();
    vinitialconfig.resize(0);
    vgoalconfig.resize(0);
    _vConfigLowerLimit.resize(0);
    _vConfigUpperLimit.resize(0);
    _vConfigResolution.resize(0);
    _sPathOptimizationPlanner = "shortcut_linear";
    _sPathOptimizationParameters.resize(0);
    _sExtraParameters.resize(0);
    _nMaxIterations = 0;
    _bCheckSelfCollisions = true;
    _fStepLength = 0.04f;
    _plannerparametersdepth = 0;
    
    // transfer data
    std::stringstream ss;
    ss << r;
    ss >> *this;
    return *this;
}

void PlannerBase::PlannerParameters::copy(boost::shared_ptr<PlannerParameters const> r)
{
    *this = *r;
}

bool PlannerBase::PlannerParameters::serialize(std::ostream& O) const
{
    O << "<_vinitialconfig>";
    FOREACHC(it, vinitialconfig)
        O << *it << " ";
    O << "</_vinitialconfig>" << endl;
    O << "<_vgoalconfig>";
    FOREACHC(it, vgoalconfig)
        O << *it << " ";
    O << "</_vgoalconfig>" << endl;
    O << "<_vconfiglowerlimit>";
    FOREACHC(it, _vConfigLowerLimit)
        O << *it << " ";
    O << "</_vconfiglowerlimit>" << endl;
    O << "<_vconfigupperlimit>";
    FOREACHC(it, _vConfigUpperLimit)
        O << *it << " ";
    O << "</_vconfigupperlimit>" << endl;
    O << "<_vconfigresolution>";
    FOREACHC(it, _vConfigResolution)
        O << *it << " ";
    O << "</_vconfigresolution>" << endl;
    
    if( !!_tWorkspaceGoal )
        O << "<_tworkspacegoal>" << *_tWorkspaceGoal << "</_tworkspacegoal>" << endl;
    
    O << "<_nmaxiterations>" << _nMaxIterations << "</_nmaxiterations>" << endl;
    O << "<_fsteplength>" << _fStepLength << "</_fsteplength>" << endl;
    O << "<_pathoptimization planner=\"" << _sPathOptimizationPlanner << "\">" << _sPathOptimizationParameters << "</_pathoptimization>" << endl;
    O << "<_bcheckselfcollisions>" << _bCheckSelfCollisions << "</_bcheckselfcollisions>" << endl;
    O << _sExtraParameters << endl;
    return !!O;
}

BaseXMLReader::ProcessElement PlannerBase::PlannerParameters::startElement(const std::string& name, const std::list<std::pair<std::string,std::string> >& atts)
{
    _ss.str(""); // have to clear the string
    if( !!__pcurreader ) {
        if( __pcurreader->startElement(name, atts) == PE_Support )
            return PE_Support;
        return PE_Ignore;
    }

    if( __processingtag.size() > 0 )
        return PE_Ignore;

    if( name=="plannerparameters" ) {
        _plannerparametersdepth++;
        return PE_Support;
    }

    if( name == "_pathoptimization" ) {
        _sslocal.reset(new std::stringstream());
        _sPathOptimizationPlanner="";
        _sPathOptimizationParameters="";
        FOREACHC(itatt,atts) {
            if( itatt->first == "planner" )
                _sPathOptimizationPlanner = itatt->second;
        }
        __pcurreader.reset(new DummyXMLReader(name,GetXMLId(),_sslocal));
        return PE_Support;
    }

    if( find(_vXMLParameters.begin(),_vXMLParameters.end(),name) == _vXMLParameters.end() ) {
        _sslocal.reset(new std::stringstream());
        *_sslocal << "<" << name << " ";
        FOREACHC(itatt, atts)
            *_sslocal << itatt->first << "=\"" << itatt->second << "\" ";
        *_sslocal << ">" << endl;
        __pcurreader.reset(new DummyXMLReader(name,GetXMLId(),_sslocal));
        return PE_Support;
    }

    if( name=="_vinitialconfig"||name=="_vgoalconfig"||name=="_vconfiglowerlimit"||name=="_vconfigupperlimit"||name=="_vconfigresolution"||name=="_tworkspacegoal"||name=="_nmaxiterations"||name=="_fsteplength"||name=="_pathoptimization"||name=="_bcheckselfcollisions" ) {
        __processingtag = name;
        return PE_Support;
    }
    return PE_Pass;
}
        
bool PlannerBase::PlannerParameters::endElement(const std::string& name)
{
    if( !!__pcurreader ) {
        if( __pcurreader->endElement(name) ) {
            boost::shared_ptr<DummyXMLReader> pdummy = boost::dynamic_pointer_cast<DummyXMLReader>(__pcurreader);
            if( !!pdummy ) {
                if( pdummy->GetFieldName() == "_pathoptimization" ) {
                    _sPathOptimizationParameters = _sslocal->str();
                    _sslocal.reset();
                }
                else {
                    *_sslocal << "</" << name << ">" << endl;
                    _sExtraParameters += _sslocal->str();
                    _sslocal.reset();
                }
            }
            __pcurreader.reset();
        }
    }
    else if( name == "plannerparameters" )
        return --_plannerparametersdepth < 0;
    else if( __processingtag.size() > 0 ) {
        if( name == "_vinitialconfig")
            vinitialconfig = vector<dReal>((istream_iterator<dReal>(_ss)), istream_iterator<dReal>());
        else if( name == "_vgoalconfig")
            vgoalconfig = vector<dReal>((istream_iterator<dReal>(_ss)), istream_iterator<dReal>());
        else if( name == "_vconfiglowerlimit")
            _vConfigLowerLimit = vector<dReal>((istream_iterator<dReal>(_ss)), istream_iterator<dReal>());
        else if( name == "_vconfigupperlimit")
            _vConfigUpperLimit = vector<dReal>((istream_iterator<dReal>(_ss)), istream_iterator<dReal>());
        else if( name == "_vconfigresolution")
            _vConfigResolution = vector<dReal>((istream_iterator<dReal>(_ss)), istream_iterator<dReal>());
        else if( name == "_tworkspacegoal") {
            _tWorkspaceGoal.reset(new Transform());
            _ss >> *_tWorkspaceGoal.get();
        }
        else if( name == "_nmaxiterations")
            _ss >> _nMaxIterations;
        else if( name == "_fsteplength")
            _ss >> _fStepLength;
        else if( name == "_bcheckselfcollisions" )
            _ss >> _bCheckSelfCollisions;
        if( name !=__processingtag )
            RAVELOG_WARN(str(boost::format("invalid tag %s!=%s\n")%name%__processingtag));
        __processingtag = "";
        return false;
    }

    return false;
}

void PlannerBase::PlannerParameters::characters(const std::string& ch)
{
    if( !!__pcurreader )
        __pcurreader->characters(ch);
    else {
        _ss.clear();
        _ss << ch;
    }
}

RAVE_API std::ostream& operator<<(std::ostream& O, const PlannerBase::PlannerParameters& v)
{
    O << "<" << v.GetXMLId() << ">" << endl;
    v.serialize(O);
    O << "</" << v.GetXMLId() << ">" << endl;
    return O;
}

class SimpleDistMetric
{
 public:
    SimpleDistMetric(RobotBasePtr robot) : _robot(robot) {
        _robot->GetActiveDOFWeights(weights);
    }
    virtual dReal Eval(const std::vector<dReal>& c0, const std::vector<dReal>& c1)
    {
        std::vector<dReal> c = c0;
        _robot->SubtractActiveDOFValues(c,c1);
        dReal dist = 0;
        for(int i=0; i < _robot->GetActiveDOF(); i++)
            dist += weights.at(i)*c.at(i)*c.at(i);
        return RaveSqrt(dist);
    }

 protected:
    RobotBasePtr _robot;
    vector<dReal> weights;
};

class SimpleSampleFunction
{
public:
    SimpleSampleFunction(RobotBasePtr robot, const boost::function<dReal(const std::vector<dReal>&, const std::vector<dReal>&)>& distmetricfn) : _robot(robot), _distmetricfn(distmetricfn) {
        _robot->GetActiveDOFLimits(lower, upper);
        range.resize(lower.size());
        for(int i = 0; i < (int)range.size(); ++i)
            range[i] = upper[i] - lower[i];
    }
    virtual bool Sample(vector<dReal>& pNewSample) {
        pNewSample.resize(lower.size());
        for (size_t i = 0; i < lower.size(); i++)
            pNewSample[i] = lower[i] + RaveRandomFloat()*range[i];
        return true;
    }

    virtual bool SampleNeigh(vector<dReal>& pNewSample, const vector<dReal>& pCurSample, dReal fRadius)
    {
        BOOST_ASSERT(pCurSample.size()==lower.size());
        sample.resize(lower.size());
        int dof = lower.size();
        for (int i = 0; i < dof; i++) {
            sample[i] = pCurSample[i] + 10.0f*fRadius*(RaveRandomFloat()-0.5f);
        }
        // normalize
        dReal fRatio = max((dReal)1e-5f,fRadius*(0.1f+0.9f*RaveRandomFloat()));
            
        //assert(_robot->ConfigDist(&_vzero[0], &_vSampleConfig[0]) < B+1);
        dReal fDist = _distmetricfn(sample,pCurSample);
        while(fDist > fRatio) {
            for (int i = 0; i < dof; i++)
                sample[i] = 0.5f*pCurSample[i]+0.5f*sample[i];
            fDist = _distmetricfn(sample,pCurSample);
        }
    
        for(int iter = 0; iter < 20; ++iter) {
            while(_distmetricfn(sample, pCurSample) < fRatio ) {
                for (int i = 0; i < dof; i++)
                    sample[i] = 1.2f*sample[i]-0.2f*pCurSample[i];
            }
        }

        pNewSample.resize(lower.size());
        for (int i = 0; i < dof; i++) {
            if( sample[i] < lower[i] )
                pNewSample[i] = lower[i];
            else if( sample[i] > upper[i] )
                pNewSample[i] = upper[i];
            else
                pNewSample[i] = sample[i];
        }

        return true;
    }

 protected:
    RobotBasePtr _robot;
    vector<dReal> lower, upper, range,sample;
    boost::function<dReal(const std::vector<dReal>&, const std::vector<dReal>&)> _distmetricfn;
};


void PlannerBase::PlannerParameters::SetRobotActiveJoints(RobotBasePtr robot)
{
    _distmetricfn = boost::bind(&SimpleDistMetric::Eval,boost::shared_ptr<SimpleDistMetric>(new SimpleDistMetric(robot)),_1,_2);
    boost::shared_ptr<SimpleSampleFunction> defaultsamplefn(new SimpleSampleFunction(robot,_distmetricfn));
    _samplefn = boost::bind(&SimpleSampleFunction::Sample,defaultsamplefn,_1);
    _sampleneighfn = boost::bind(&SimpleSampleFunction::SampleNeigh,defaultsamplefn,_1,_2,_3);
    _setstatefn = boost::bind(&RobotBase::SetActiveDOFValues,robot,_1,false);
    _getstatefn = boost::bind(&RobotBase::GetActiveDOFValues,robot,_1);
    _diffstatefn = boost::bind(&RobotBase::SubtractActiveDOFValues,robot,_1,_2);
    _bCheckSelfCollisions = robot->GetActiveDOF() != robot->GetAffineDOF();
    robot->GetActiveDOFLimits(_vConfigLowerLimit,_vConfigUpperLimit);
    robot->GetActiveDOFResolutions(_vConfigResolution);
    robot->GetActiveDOFValues(vinitialconfig);
    BOOST_ASSERT((int)_vConfigResolution.size()==robot->GetActiveDOF());
}

bool PlannerBase::InitPlan(RobotBasePtr pbase, std::istream& isParameters)
{
    RAVELOG_WARN(str(boost::format("using default planner parameters structure to de-serialize parameters data inside %s, information might be lost!! Please define a InitPlan(robot,stream) function!\n")%GetXMLId()));
    boost::shared_ptr<PlannerParameters> localparams(new PlannerParameters());
    isParameters >> *localparams;
    return InitPlan(pbase,localparams);
}

bool PlannerBase::_OptimizePath(RobotBasePtr probot, TrajectoryBasePtr ptraj)
{
    if( GetParameters()->_sPathOptimizationPlanner.size() == 0 )
        return true;
    PlannerBasePtr planner = RaveCreatePlanner(GetEnv(), GetParameters()->_sPathOptimizationPlanner);
    if( !planner )
        return false;
    PlannerParametersPtr params(new PlannerParameters());
    params->copy(GetParameters());
    params->_sExtraParameters += GetParameters()->_sPathOptimizationParameters;
    params->_sPathOptimizationPlanner = "";
    params->_sPathOptimizationParameters = "";
    params->_nMaxIterations = 0; // have to reset since path optimizers also use it and new parameters could be in extra parameters
    if( !planner->InitPlan(probot, params) )
        return false;
    return planner->PlanPath(ptraj);
}

#ifdef _WIN32
const char *strcasestr(const char *s, const char *find)
{
    register char c, sc;
    register size_t len;

    if ((c = *find++) != 0) {
	c = tolower((unsigned char)c);
	len = strlen(find);
	do {
	    do {
		if ((sc = *s++) == 0)
		    return (NULL);
	    } while ((char)tolower((unsigned char)sc) != c);
	} while (strnicmp(s, find, len) != 0);
	s--;
    }
    return ((char *) s);
}
#endif

#define LIBXML_SAX1_ENABLED
#include <libxml/globals.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h> // only for xmlNewInputFromFile()
#include <libxml/tree.h>

#include <libxml/debugXML.h>
#include <libxml/xmlmemory.h>

namespace LocalXML {

void RaveXMLErrorFunc(void *ctx, const char *msg, ...)
{
    va_list args;

    va_start(args, msg);
    RAVELOG_ERRORA("XML Parse error: ");
    vprintf(msg,args);
    va_end(args);
}

struct XMLREADERDATA
{
    XMLREADERDATA(BaseXMLReaderPtr preader, xmlParserCtxtPtr ctxt) : _preader(preader), _ctxt(ctxt) {}
    BaseXMLReaderPtr _preader, _pdummy;
    xmlParserCtxtPtr _ctxt;
};

void DefaultStartElementSAXFunc(void *ctx, const xmlChar *name, const xmlChar **atts)
{
    std::list<std::pair<std::string,std::string> > listatts;
    if( atts != NULL ) {
        for (int i = 0;(atts[i] != NULL);i+=2) {
            listatts.push_back(make_pair(string((const char*)atts[i]),string((const char*)atts[i+1])));
            std::transform(listatts.back().first.begin(), listatts.back().first.end(), listatts.back().first.begin(), ::tolower);
        }
    }

    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    string s = (const char*)name;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if( !!pdata->_pdummy ) {
        RAVELOG_VERBOSE(str(boost::format("unknown field %s\n")%s));
        pdata->_pdummy->startElement(s,listatts);
    }
    else {
        if( ((XMLREADERDATA*)ctx)->_preader->startElement(s, listatts) != BaseXMLReader::PE_Support ) {
            // not handling, so create a temporary class to handle it
            pdata->_pdummy.reset(new DummyXMLReader(s,"(libxml)"));
        }
    }
}

void DefaultEndElementSAXFunc(void *ctx, const xmlChar *name)
{
    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    string s = (const char*)name;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if( !!pdata->_pdummy ) {
        if( pdata->_pdummy->endElement(s) ) {
            pdata->_pdummy.reset();
        }
    }
    else {
        if( pdata->_preader->endElement(s) ) {
            //RAVEPRINT(L"%s size read %d\n", name, data->_ctxt->input->consumed);
            xmlStopParser(pdata->_ctxt);
        }
    }
}

void DefaultCharactersSAXFunc(void *ctx, const xmlChar *ch, int len)
{
    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    if( !!pdata->_pdummy )
        pdata->_pdummy->characters(string((const char*)ch, len));
    else
        pdata->_preader->characters(string((const char*)ch, len));
}

bool xmlDetectSAX2(xmlParserCtxtPtr ctxt)
{
    if (ctxt == NULL)
        return false;
#ifdef LIBXML_SAX1_ENABLED
    if ((ctxt->sax) &&  (ctxt->sax->initialized == XML_SAX2_MAGIC) &&
        ((ctxt->sax->startElementNs != NULL) ||
         (ctxt->sax->endElementNs != NULL))) ctxt->sax2 = 1;
#else
    ctxt->sax2 = 1;
#endif /* LIBXML_SAX1_ENABLED */

    ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
    ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
    ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
    if ((ctxt->str_xml==NULL) || (ctxt->str_xmlns==NULL) || 
        (ctxt->str_xml_ns == NULL)) {
        return false;
    }

    return true;
}

bool ParseXMLData(BaseXMLReaderPtr preader, const char* buffer, int size)
{
    static xmlSAXHandler s_DefaultSAXHandler = {0};
    if( size <= 0 ) {
        size = strlen(buffer);
    }
    if( !s_DefaultSAXHandler.initialized ) {
        // first time, so init
        s_DefaultSAXHandler.startElement = DefaultStartElementSAXFunc;
        s_DefaultSAXHandler.endElement = DefaultEndElementSAXFunc;
        s_DefaultSAXHandler.characters = DefaultCharactersSAXFunc;
        s_DefaultSAXHandler.error = RaveXMLErrorFunc;
        s_DefaultSAXHandler.initialized = 1;
    }

    xmlSAXHandlerPtr sax = &s_DefaultSAXHandler;
    int ret = 0;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (ctxt == NULL) {
        return false;
    }
    if (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler) {
        xmlFree(ctxt->sax);
    }
    ctxt->sax = sax;
    xmlDetectSAX2(ctxt);

    XMLREADERDATA reader(preader, ctxt);
    ctxt->userData = &reader;
    
    xmlParseDocument(ctxt);
    
    if (ctxt->wellFormed) {
        ret = 0;
    }
    else {
        if (ctxt->errNo != 0) {
            ret = ctxt->errNo;
        }
        else {
            ret = -1;
        }
    }
    if (sax != NULL) {
        ctxt->sax = NULL;
    }
    if (ctxt->myDoc != NULL) {
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    xmlFreeParserCtxt(ctxt);
    
    return ret==0;
}

}

RAVE_API std::istream& operator>>(std::istream& I, PlannerBase::PlannerParameters& pp)
{
    if( !!I) {
        stringbuf buf;
        stringstream::streampos pos = I.tellg();
        I.get(buf, 0); // get all the data, yes this is inefficient, not sure if there anyway to search in streams

        string pbuf = buf.str();
        const char* p = strcasestr(pbuf.c_str(), "</PlannerParameters>");
        int ppsize=-1;
        if( p != NULL ) {
            I.clear();
            ppsize=(p-pbuf.c_str())+20;
            I.seekg((size_t)pos+ppsize);
        }
        else
            throw openrave_exception(str(boost::format("error, failed to find </PlannerParameters> in %s")%buf.str()),ORE_InvalidArguments);
        pp._plannerparametersdepth = 0;
        LocalXML::ParseXMLData(PlannerBase::PlannerParametersPtr(&pp,null_deleter()), pbuf.c_str(), ppsize);
    }

    return I;
}

EnvironmentBase::EnvironmentBase()
{
    if( !RaveGlobalState() ) {
        RAVELOG_WARN("OpenRAVE global state not initialized! Need to call RaveInitialize before any OpenRAVE services can be used. For now, initializing with default parameters.\n");
        RaveInitialize(true);
    }
    RaveGlobal::instance()->RegisterEnvironment(this);
}

EnvironmentBase::~EnvironmentBase()
{
    RaveGlobal::instance()->UnregisterEnvironment(this);
}

InterfaceBase::InterfaceBase(InterfaceType type, EnvironmentBasePtr penv) : __type(type), __penv(penv)
{
    RaveInitializeFromState(penv->GlobalState()); // make sure global state is set
    RegisterCommand("help",boost::bind(&InterfaceBase::_GetCommandHelp,this,_1,_2),
                    "display help commands.");
    __description = "Not documented yet.";
}

InterfaceBase::~InterfaceBase()
{
    boost::mutex::scoped_lock lock(_mutexInterface);
    __mapCommands.clear();
    __pUserData.reset();
    __mapReadableInterfaces.clear();
    __penv.reset();
}

bool InterfaceBase::Clone(InterfaceBaseConstPtr preference, int cloningoptions)
{
    if( !preference )
        throw openrave_exception("invalid cloning reference",ORE_InvalidArguments);
    __pUserData = preference->__pUserData;
    __strxmlfilename = preference->__strxmlfilename;
    __mapReadableInterfaces = preference->__mapReadableInterfaces;
    return true;
}

bool InterfaceBase::SendCommand(ostream& sout, istream& sinput)
{
    string cmd;
    sinput >> cmd;
    if( !sinput ) {
        throw openrave_exception("invalid command",ORE_InvalidArguments);
    }
    boost::shared_ptr<InterfaceCommand> interfacecmd;
    {
        boost::mutex::scoped_lock lock(_mutexInterface);
        CMDMAP::iterator it = __mapCommands.find(cmd);
        if( it == __mapCommands.end() ) {
            throw openrave_exception(str(boost::format("failed to find command '%s' in interface %s\n")%cmd.c_str()%GetXMLId()),ORE_CommandNotSupported);
        }
        interfacecmd = it->second;
    }
    if( !interfacecmd->fn(sout,sinput) ) {
        RAVELOG_VERBOSE(str(boost::format("command failed in problem %s: %s\n")%GetXMLId()%cmd));
        return false;
    }
    return true;
}

void InterfaceBase::RegisterCommand(const std::string& cmdname, InterfaceBase::InterfaceCommandFn fncmd, const std::string& strhelp)
{
    boost::mutex::scoped_lock lock(_mutexInterface);
    if( cmdname.size() == 0 || !IsValidName(cmdname) || stricmp(cmdname.c_str(),"commands") == 0 ) {
        throw openrave_exception(str(boost::format("command '%s' invalid")%cmdname),ORE_InvalidArguments);
    }
    if( __mapCommands.find(cmdname) != __mapCommands.end() ) {
        throw openrave_exception(str(boost::format("command '%s' already registered")%cmdname),ORE_InvalidArguments);
    }
    __mapCommands[cmdname] = boost::shared_ptr<InterfaceCommand>(new InterfaceCommand(fncmd, strhelp));
}

void InterfaceBase::UnregisterCommand(const std::string& cmdname)
{
    boost::mutex::scoped_lock lock(_mutexInterface);
    CMDMAP::iterator it = __mapCommands.find(cmdname);
    if( it != __mapCommands.end() ) {
        __mapCommands.erase(it);
    }
}

bool InterfaceBase::_GetCommandHelp(std::ostream& o, std::istream& sinput) const
{
    boost::mutex::scoped_lock lock(_mutexInterface);
    string cmd;
    sinput >> cmd;
    CMDMAP::const_iterator it;
    if( !!sinput && cmd == "commands" ) {
        for(it = __mapCommands.begin(); it != __mapCommands.end(); ++it) {
            o << it->first << " ";
        }
    }
    else {
        it = __mapCommands.find(cmd);
        if( !sinput || it == __mapCommands.end() ) {
            // display full help string
            o << endl << GetXMLId() << " Commands" << endl;
            for(size_t i = 0; i < GetXMLId().size(); ++i) {
                o << "=";
            }
            o << "=========" << endl << endl;
            for(it = __mapCommands.begin(); it != __mapCommands.end(); ++it) {
                o << endl << "**" << it->first << "**" << endl;
                for(size_t i = 0; i < it->first.size()+4; ++i) {
                    o << "~";
                }
                o << endl << endl << it->second->help << endl;
            }
        }
        else {
            o << it->second->help;
        }
    }
    return true;
}

bool SensorBase::SensorData::serialize(std::ostream& O) const
{
    RAVELOG_WARNA("SensorData XML serialization not implemented\n");
    return true;
}

bool SensorBase::LaserSensorData::serialize(std::ostream& O) const
{
    RAVELOG_WARNA("LaserSensorData XML serialization not implemented\n");
    return true;
}

bool SensorBase::CameraSensorData::serialize(std::ostream& O) const
{
    RAVELOG_WARNA("CameraSensorData XML serialization not implemented\n");
    return true;
}

/// SimpleSensorSystem
SimpleSensorSystem::SimpleXMLReader::SimpleXMLReader(boost::shared_ptr<XMLData> p) : _pdata(p)
{
}

BaseXMLReader::ProcessElement SimpleSensorSystem::SimpleXMLReader::startElement(const std::string& name, const std::list<std::pair<std::string,std::string> >& atts)
{
    ss.str("");
    if( name != _pdata->GetXMLId() && name != "offsetlink" && name != "id" && name != "sid" && name != "translation" && name != "rotationmat" && name != "rotationaxis" && name != "quat" && name != "pretranslation" && name != "prerotation" && name != "prerotationaxis" && name != "prequat" ) {
        return PE_Pass;
    }
    return PE_Support;
}

bool SimpleSensorSystem::SimpleXMLReader::endElement(const std::string& name)
{
    if( name == "offsetlink" )
        ss >> _pdata->strOffsetLink;
    else if( name == "id" )
        ss >> _pdata->id;
    else if( name == "sid" )
        ss >> _pdata->sid;
    else if( name == "translation" )
        ss >> _pdata->transOffset.trans.x >> _pdata->transOffset.trans.y >> _pdata->transOffset.trans.z;
    else if( name == "rotationmat" ) {
        TransformMatrix m;
        ss >> m.m[0] >> m.m[1] >> m.m[2] >> m.m[4] >> m.m[5] >> m.m[6] >> m.m[8] >> m.m[9] >> m.m[10];
        _pdata->transOffset.rot = Transform(m).rot;
    }
    else if( name == "rotationaxis" ) {
        Vector axis; dReal fang;
        ss >> axis.x >> axis.y >> axis.z >> fang;
        _pdata->transOffset.rotfromaxisangle(axis.normalize3(),fang*dReal(PI/180.0));
    }
    else if( name == "quat" )
        ss >> _pdata->transOffset.rot;
    else if( name == "pretranslation")
        ss >> _pdata->transPreOffset.trans.x >> _pdata->transPreOffset.trans.y >> _pdata->transPreOffset.trans.z;
    else if( name == "prerotationmat") {
        TransformMatrix m;
        ss >> m.m[0] >> m.m[1] >> m.m[2] >> m.m[4] >> m.m[5] >> m.m[6] >> m.m[8] >> m.m[9] >> m.m[10];
        _pdata->transPreOffset.rot = Transform(m).rot;
    }
    else if( name == "prerotationaxis") {
        Vector axis; dReal fang;
        ss >> axis.x >> axis.y >> axis.z >> fang;
        _pdata->transPreOffset.rotfromaxisangle(axis,fang*dReal(PI/180.0));
    }
    else if( name == "prequat")
        ss >> _pdata->transPreOffset.rot;
    else if( name == tolowerstring(_pdata->GetXMLId()) )
        return true;
        
    if( !ss )
        RAVELOG_WARNA(str(boost::format("error parsing %s\n")%name));
    return false;
}

void SimpleSensorSystem::SimpleXMLReader::characters(const std::string& ch)
{
    ss.clear();
    ss << ch;
}

BaseXMLReaderPtr SimpleSensorSystem::CreateXMLReaderId(const string& xmlid, InterfaceBasePtr ptr, const std::list<std::pair<std::string,std::string> >& atts)
{
    return BaseXMLReaderPtr(new SimpleXMLReader(boost::shared_ptr<XMLData>(new XMLData(xmlid))));
}

boost::shared_ptr<void> SimpleSensorSystem::RegisterXMLReaderId(EnvironmentBasePtr penv, const string& xmlid)
{
    return RaveRegisterXMLReader(PT_KinBody,xmlid, boost::bind(&SimpleSensorSystem::CreateXMLReaderId,xmlid, _1,_2));
}

SimpleSensorSystem::SimpleSensorSystem(const std::string& xmlid, EnvironmentBasePtr penv) : SensorSystemBase(penv), _expirationtime(2000000), _bShutdown(false), _threadUpdate(boost::bind(&SimpleSensorSystem::_UpdateBodiesThread,this))
{
    _xmlid = xmlid;
    std::transform(_xmlid.begin(), _xmlid.end(), _xmlid.begin(), ::tolower);
}

SimpleSensorSystem::~SimpleSensorSystem()
{
    Reset();
    _bShutdown = true;
    _threadUpdate.join();
}

void SimpleSensorSystem::Reset()
{
    boost::mutex::scoped_lock lock(_mutex);
    _mapbodies.clear();        
}

void SimpleSensorSystem::AddRegisteredBodies(const std::vector<KinBodyPtr>& vbodies)
{
    // go through all bodies in the environment and check for mocap data
    FOREACHC(itbody, vbodies) {
        boost::shared_ptr<XMLData> pmocapdata = boost::dynamic_pointer_cast<XMLData>((*itbody)->GetReadableInterface(_xmlid));
        if( !!pmocapdata ) {
            KinBody::ManageDataPtr p = AddKinBody(*itbody, pmocapdata);
            if( !!p )
                p->Lock(true);
        }
    }
}

KinBody::ManageDataPtr SimpleSensorSystem::AddKinBody(KinBodyPtr pbody, XMLReadableConstPtr _pdata)
{
    BOOST_ASSERT(pbody->GetEnv()==GetEnv());
    boost::shared_ptr<XMLData const> pdata = boost::static_pointer_cast<XMLData const>(_pdata);
    if( !pdata ) {
        pdata = boost::dynamic_pointer_cast<XMLData const>(pbody->GetReadableInterface(_xmlid));
        if( !pdata ) {
            RAVELOG_VERBOSE(str(boost::format("failed to find manage data for body %s\n")%pbody->GetName()));
            return KinBody::ManageDataPtr();
        }
    }

    boost::mutex::scoped_lock lock(_mutex);
    if( _mapbodies.find(pbody->GetEnvironmentId()) != _mapbodies.end() ) {
        RAVELOG_WARNA(str(boost::format("body %s already added\n")%pbody->GetName()));
        return KinBody::ManageDataPtr();
    }
    
    boost::shared_ptr<BodyData> b = CreateBodyData(pbody, pdata);
    b->lastupdated = GetMicroTime();
    _mapbodies[pbody->GetEnvironmentId()] = b;
    RAVELOG_VERBOSE(str(boost::format("system adding body %s (%s), total: %d\n")%pbody->GetName()%pbody->GetXMLFilename()%_mapbodies.size()));
    SetManageData(pbody,b);
    return b;
}

bool SimpleSensorSystem::RemoveKinBody(KinBodyPtr pbody)
{
    boost::mutex::scoped_lock lock(_mutex);
    bool bSuccess = _mapbodies.erase(pbody->GetEnvironmentId())>0;
    RAVELOG_VERBOSE(str(boost::format("system removing body %s %s\n")%pbody->GetName()%(bSuccess?"succeeded":"failed")));
    return bSuccess;
}

bool SimpleSensorSystem::IsBodyPresent(KinBodyPtr pbody)
{
    boost::mutex::scoped_lock lock(_mutex);
    return _mapbodies.find(pbody->GetEnvironmentId()) != _mapbodies.end();
}

bool SimpleSensorSystem::EnableBody(KinBodyPtr pbody, bool bEnable)
{
    boost::mutex::scoped_lock lock(_mutex);
    BODIES::iterator it = _mapbodies.find(pbody->GetEnvironmentId());
    if( it == _mapbodies.end() ) {
        RAVELOG_WARNA("trying to %s body %s that is not in system\n", bEnable?"enable":"disable", pbody->GetName().c_str());
        return false;
    }

    it->second->bEnabled = bEnable;
    return true;
}

bool SimpleSensorSystem::SwitchBody(KinBodyPtr pbody1, KinBodyPtr pbody2)
{
    //boost::mutex::scoped_lock lock(_mutex);
    BODIES::iterator it = _mapbodies.find(pbody1->GetEnvironmentId());
    boost::shared_ptr<BodyData> pb1,pb2;
    if( it != _mapbodies.end() )
        pb1 = it->second;
    it = _mapbodies.find(pbody2->GetEnvironmentId());
    if( it != _mapbodies.end() )
        pb2 = it->second;

    if( !pb1 || !pb2 )
        return false;

    if( !!pb1 )
        pb1->SetBody(pbody2);
    if( !!pb2 )
        pb2->SetBody(pbody1);

    return true;
}

boost::shared_ptr<SimpleSensorSystem::BodyData> SimpleSensorSystem::CreateBodyData(KinBodyPtr pbody, boost::shared_ptr<XMLData const> pdata)
{
    boost::shared_ptr<XMLData> pnewdata(new XMLData(_xmlid));
    pnewdata->copy(pdata);
    return boost::shared_ptr<BodyData>(new BodyData(RaveInterfaceCast<SimpleSensorSystem>(shared_from_this()),pbody, pnewdata));
}

void SimpleSensorSystem::_UpdateBodies(list<SimpleSensorSystem::SNAPSHOT>& listbodies)
{
    EnvironmentMutex::scoped_lock lockenv(GetEnv()->GetMutex()); // always lock environment to preserve mutex order
    uint64_t curtime = GetMicroTime();
    if( listbodies.size() > 0 ) {

        FOREACH(it, listbodies) {
            BOOST_ASSERT( it->first->IsEnabled() );

            KinBody::LinkPtr plink = it->first->GetOffsetLink();
            if( !plink )
                continue;

            // transform with respect to offset link
            TransformMatrix tlink = plink->GetTransform();
            TransformMatrix tbase = plink->GetParent()->GetTransform();
            TransformMatrix toffset = tbase * tlink.inverse() * it->first->_initdata->transOffset;
            TransformMatrix tfinal = toffset * it->second*it->first->_initdata->transPreOffset;
            
            plink->GetParent()->SetTransform(tfinal);
            it->first->lastupdated = curtime;
            it->first->tnew = it->second;
            
            //RAVELOG_DEBUGA("%f %f %f\n", tfinal.trans.x, tfinal.trans.y, tfinal.trans.z);
            
            if( !it->first->IsPresent() )
                RAVELOG_VERBOSEA(str(boost::format("updating body %s\n")%plink->GetParent()->GetName()));
            it->first->bPresent = true;
        }
    }

    boost::mutex::scoped_lock lock(_mutex);
    BODIES::iterator itbody = _mapbodies.begin();
    while(itbody != _mapbodies.end()) {
        KinBody::LinkPtr plink = itbody->second->GetOffsetLink();
        if( !!plink && plink->GetParent()->GetEnvironmentId()==0 ) {
            _mapbodies.erase(itbody++);
            continue;
        }
        else if( curtime-itbody->second->lastupdated > _expirationtime ) {


            if( !itbody->second->IsLocked() ) {
                if( !!plink ) {
                    //RAVELOG_VERBOSEA(str(boost::format("object %s expired %fs\n")%plink->GetParent()->GetName()*((curtime-itbody->second->lastupdated)*1e-6f)));
                    GetEnv()->Remove(plink->GetParent());
                }
                _mapbodies.erase(itbody++);
                continue;
            }
                
            if( itbody->second->IsPresent() && !!plink )
                RAVELOG_VERBOSEA(str(boost::format("body %s not present\n")%plink->GetParent()->GetName()));
            itbody->second->bPresent = false;
        }

        ++itbody;
    }
}

void SimpleSensorSystem::_UpdateBodiesThread()
{
    list< SNAPSHOT > listbodies;

    while(!_bShutdown) {
        {
            _UpdateBodies(listbodies);
        }
        Sleep(10); // 10ms
    }
}

RAVE_API void RaveInitRandomGeneration(uint32_t seed)
{
    init_genrand(seed);
}

RAVE_API uint32_t RaveRandomInt()
{
    return genrand_int32();
}

RAVE_API void RaveRandomInt(int n, std::vector<int>& v)
{
    v.resize(n);
    FOREACH(it, v) *it = genrand_int32();
}

RAVE_API float RaveRandomFloat()
{
    return (float)genrand_real1();
}

RAVE_API void RaveRandomFloat(int n, std::vector<float>& v)
{
    v.resize(n);
    FOREACH(it, v) *it = (float)genrand_real1();
}

RAVE_API double RaveRandomDouble()
{
    return genrand_res53();
}
 
RAVE_API void RaveRandomDouble(int n, std::vector<double>& v)
{
    v.resize(n);
    FOREACH(it, v) *it = genrand_res53();
}

std::string GetMD5HashString(const std::string& s)
{
    if( s.size() == 0 )
        return "";

    md5_state_t state;
	md5_byte_t digest[16];
	
	md5_init(&state);
	md5_append(&state, (const md5_byte_t *)s.c_str(), s.size());
	md5_finish(&state, digest);
    string hex_output;
    hex_output.resize(32);
    for (int di = 0; di < 16; ++di) {
        int n = (digest[di]&0xf);
        hex_output[2*di+1] = n > 9 ? ('a'+n-10) : ('0'+n);
        n = (digest[di]&0xf0)>>4;
        hex_output[2*di+0] = n > 9 ? ('a'+n-10) : ('0'+n);
    }
    return hex_output;
}

std::string GetMD5HashString(const std::vector<uint8_t>& v)
{
    if( v.size() == 0 )
        return "";

    md5_state_t state;
	md5_byte_t digest[16];
	
	md5_init(&state);
	md5_append(&state, (const md5_byte_t *)&v[0], v.size());
	md5_finish(&state, digest);
    string hex_output;
    hex_output.resize(32);
    for (int di = 0; di < 16; ++di) {
        int n = (digest[di]&0xf);
        hex_output[2*di+0] = n > 9 ? ('a'+n-10) : ('0'+n);
        n = (digest[di]&0xf0)>>4;
        hex_output[2*di+1] = n > 9 ? ('a'+n-10) : ('0'+n);
    }
    return hex_output;
}

} // end namespace OpenRAVE

/** Copyright (c) 2013, Jonathan Bohren, all rights reserved. 
 * This software is released under the BSD 3-clause license, for the details of
 * this license, please see LICENSE.txt at the root of this repository. 
 */

#include <rtt/plugin/ServicePlugin.hpp>

#include <rtt/deployment/ComponentLoader.hpp>

#include "ros_interface_service.h"

#include <rtt_roscomm/rtt_rostopic.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <conman/hook.h>
#include <iomanip>


using namespace conman_ros;

ROSInterfaceService::ROSInterfaceService(RTT::TaskContext* owner) :
  RTT::Service("conman_ros",owner),
  scheme(dynamic_cast<conman::Scheme*>(owner))
{ 
  // Make sure we're attached to a scheme
  if(!scheme) { 
    std::string err_text = "Attmpted to load the Conman ROS interface on a component which isn't a scheme!";
    RTT::log(RTT::Error) << err_text << RTT::endlog();
    throw std::runtime_error(err_text);
  }

  // Connect operation callers
  RTT::log(RTT::Debug) << "Connecting conamn_ros operation callers..." << RTT::endlog();
  getBlocks = scheme->getOperation("getBlocks");
  getGroups = scheme->getOperation("getGroups");
  switchBlocks = scheme->getOperation("switchBlocks");

  // Create ros-control operation bindings
  RTT::log(RTT::Debug) << "Creating ros_control service servers..." << RTT::endlog();
  roscontrol = owner->provides("roscontrol");
  roscontrol->addOperation("listControllerTypes", &ROSInterfaceService::listControllerTypesCB, this);
  roscontrol->addOperation("listControllers", &ROSInterfaceService::listControllersCB, this);
  roscontrol->addOperation("loadController", &ROSInterfaceService::loadControllerCB, this);
  roscontrol->addOperation("reloadControllerLibraries", &ROSInterfaceService::reloadControllerLibrariesCB, this);
  roscontrol->addOperation("switchController", &ROSInterfaceService::switchControllerCB, this);
  roscontrol->addOperation("unloadController", &ROSInterfaceService::unloadControllerCB, this);

  // Load the rosservice service
  RTT::log(RTT::Debug) << "Getting rtt_roscomm service service..." << RTT::endlog();
  rosservice = owner->getProvider<rtt_rosservice::ROSService>("rosservice");

  RTT::log(RTT::Debug) << "Connecting ros_control service servers..." << RTT::endlog();
  rosservice->connect("roscontrol.listControllerTypes",
                     "controller_manager/list_controller_types",
                     "controller_manager_msgs/ListControllerTypes");

  rosservice->connect("roscontrol.listControllers",
                     "controller_manager/list_controllers",
                     "controller_manager_msgs/ListControllers");
  
  rosservice->connect("roscontrol.loadController",
                     "controller_manager/load_controller",
                     "controller_manager_msgs/LoadController");

  rosservice->connect("roscontrol.reloadControllerLibraries",
                     "controller_manager/reload_controller_libraries",
                     "controller_manager_msgs/ReloadControllerLibraries");

  rosservice->connect("roscontrol.switchController",
                     "controller_manager/switch_controller",
                     "controller_manager_msgs/SwitchController");

  rosservice->connect("roscontrol.unloadController",
                     "controller_manager/unload_controller",
                     "controller_manager_msgs/UnloadController"); 

  // Introspection
  introspection = owner->provides("introspection");
  introspection->addOperation("broadcastGraph", &ROSInterfaceService::broadcastGraph, this, RTT::ClientThread)
    .doc("Broadcast a graphviz representation of the scheme and its members.");
  introspection->addPort("dotcode_out", dotcode_out_);
  dotcode_out_.createStream(rtt_roscomm::topic("~"+owner->getName()+"/dotcode"));

}

bool ROSInterfaceService::listControllerTypesCB(
    controller_manager_msgs::ListControllerTypes::Request &req,
    controller_manager_msgs::ListControllerTypes::Response& resp)
{

  return false;
}
bool ROSInterfaceService::listControllersCB(
    controller_manager_msgs::ListControllers::Request &req,
    controller_manager_msgs::ListControllers::Response& resp)
{
  const std::vector<std::string> block_names = getBlocks();
  const std::vector<std::string> group_names = getGroups();

  resp.controller.reserve(block_names.size() + group_names.size());

  for(std::vector<std::string>::const_iterator it = block_names.begin();
      it != block_names.end();
      ++it)
  {
    RTT::TaskContext *block_task = scheme->getPeer(*it);
    controller_manager_msgs::ControllerState cs;
    cs.name = *it;
    cs.type = "OROCOS COMPONENT";
    cs.state = (block_task->getTaskState() == RTT::TaskContext::Running) ? "running" : "stopped";
    resp.controller.push_back(cs);
  }

  for(std::vector<std::string>::const_iterator it = group_names.begin();
      it != group_names.end();
      ++it)
  {
    controller_manager_msgs::ControllerState cs;
    cs.name = *it;
    cs.type = "CONMAN GROUP";
    resp.controller.push_back(cs);
  }

  return true;
}
bool ROSInterfaceService::loadControllerCB(
    controller_manager_msgs::LoadController::Request &req,
    controller_manager_msgs::LoadController::Response& resp)
{
  return false;
}
bool ROSInterfaceService::reloadControllerLibrariesCB(
    controller_manager_msgs::ReloadControllerLibraries::Request &req,
    controller_manager_msgs::ReloadControllerLibraries::Response& resp)
{
  return false;
}
bool ROSInterfaceService::switchControllerCB(
    controller_manager_msgs::SwitchController::Request &req,
    controller_manager_msgs::SwitchController::Response& resp)
{
  RTT::log(RTT::Debug) << "Handling ros_control switch controllers request..." << RTT::endlog();
  resp.ok = switchBlocks(
      req.stop_controllers,
      req.start_controllers,
      req.strictness == controller_manager_msgs::SwitchController::Request::STRICT,
      false);

  return true;
}
bool ROSInterfaceService::unloadControllerCB(
    controller_manager_msgs::UnloadController::Request &req,
    controller_manager_msgs::UnloadController::Response& resp)
{
  return false;
}


// Graphviz record labels can't have dots in them
std::string sanitize(const std::string &unclean) {
  std::string clean(unclean);
  boost::replace_all(clean, ".", "_DOT_");
  return clean;
}

void ROSInterfaceService::broadcastGraph()
{
  using namespace conman::graph;

  RTT::Logger::In in("Scheme::broadcastGraph");

  // Create stringstreams for generating dotcode
  std::ostringstream main_stream;

  // Construct preamble
  main_stream << "\
    digraph " << this->getName() << " {\
      splines=true;\
      overlap=false;\
      rankdir=LR;\
      nodesep=0.5;\
      ranksep=1.5;\
      fontname=\"sans\";\
      node [style=\"rounded,filled\",fontsize=15,color=\"#777777\",fillcolor=\"#eeeeee\"]\
      ";

  // Get blocks
  std::vector<conman::BlockDescription> block_descriptions;
  scheme->getBlockDescriptions(block_descriptions);

  for(std::vector<conman::BlockDescription>::const_iterator block_it=block_descriptions.begin();
      block_it != block_descriptions.end();
      ++block_it)
  {
    std::ostringstream &oss = main_stream;
    oss << block_it->name + "[shape=record, label=\"\\N|{{";

    for(std::vector<std::string>::const_iterator port_name_it=block_it->input_ports.begin();
        port_name_it != block_it->input_ports.end();
        ++port_name_it)
    {
      oss << "<" << sanitize(*port_name_it) << ">" << *port_name_it << "|";
    }
    oss << "}| |{";
    for(std::vector<std::string>::const_iterator port_name_it=block_it->output_ports.begin();
        port_name_it != block_it->output_ports.end();
        ++port_name_it)
    {
      oss << "<" << sanitize(*port_name_it) << ">" << *port_name_it << "|";
    }

    oss << "}}";

    // Add statistics
    RTT::TaskContext *task = scheme->getPeer(block_it->name);
    boost::shared_ptr<conman::Hook> hook = conman::Hook::GetHook(task);
    RTT::Seconds 
      pavg = hook->getPeriodAvg(),
      pmin = hook->getPeriodMin(), 
      pmax = hook->getPeriodMax(), 
      pvar = hook->getPeriodVar();
    RTT::Seconds 
      davg = hook->getDurationAvg(),
      dmin = hook->getDurationMin(), 
      dmax = hook->getDurationMax(), 
      dvar = hook->getDurationVar();

    double fraction  = davg/pavg;

    // |{{0.0001|205.7}|{0.00112 +/- 1E-7 (85%)|800.4 +/- 3} | {0.002|855.5}}

    oss << boost::str(boost::format("|{{%1.2e|%1.2e}|{%1.2e +/- %1.1e|%1.2e +/- %1.1e (%3.1f%%)}|{%1.2e|%1.2e}}") 
                      % pmin % dmin % pavg % pvar % davg % dvar % (100.0*fraction) % pmax % dmax);
    //oss << "|{ period: " << pmin << "|" << pavg << " +/- " << std::setprecision(2) << pvar << "|" << pmax << "}";
    //oss << "|{ duration: " << dmin << "|" << davg << " +/- " << dvar << "|" << dmax << "| " << fraction*100.0 << "% }";
    oss << "\"";

#if 0
    oss << "fillcolor=\"#" << std::hex 
      << int(255*fraction) 
      << int(255*(1.0-fraction)) 
      << int(255*(1.0-fraction)) << std::dec << "\"";
#endif

    if(task->isRunning()) {
      oss << " fillcolor=\"" << 0.66*(1.0-std::min(std::max(fraction,0.0),1.0)) << " 0.6 0.8 \"";
      oss << " color=\"" << 0.66*(1.0-std::min(std::max(fraction,0.0),1.0)) << " 0.6 0.3 \"";
    } else {
      oss << " fillcolor=\"0.0 0.0 0.8\" color=\"0.0 0.0 0.3\"";
    }

    oss << "];";
  }

  std::vector<conman::ConnectionDescription> conn_descriptions;
  scheme->getConnectionDescriptions(conn_descriptions);

  for(std::vector<conman::ConnectionDescription>::const_iterator conn_it=conn_descriptions.begin();
      conn_it != conn_descriptions.end();
      ++conn_it)
  {
    std::ostringstream &oss = main_stream;
    oss << conn_it->source << ":" << sanitize(conn_it->source_port) << " -> " << conn_it->sink << ":" << sanitize(conn_it->sink_port) << ";";
  }

  main_stream << "}";

  std_msgs::String msg;
  msg.data = main_stream.str();
  
  dotcode_out_.write(msg);
}

ORO_SERVICE_NAMED_PLUGIN(conman_ros::ROSInterfaceService, "conman_ros");



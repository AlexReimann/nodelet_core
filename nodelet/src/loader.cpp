/*
 * Copyright (c) 2010, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <nodelet/loader.h>
#include <nodelet/nodelet.h>
#include <nodelet/detail/callback_queue.h>
#include <nodelet/detail/callback_queue_manager.h>
#include <pluginlib/class_loader.h>

#include <ros/ros.h>
#include <ros/callback_queue.h>
#include <nodelet/NodeletLoad.h>
#include <nodelet/NodeletList.h>
#include <nodelet/NodeletUnload.h>

#include <sstream>
#include <map>
#include <boost/shared_ptr.hpp>

namespace nodelet
{

namespace detail
{
class LoaderROS
{
public:
  LoaderROS(Loader* parent, const ros::NodeHandle& nh)
  : parent_(parent)
  , nh_(nh)
  , bond_spinner_(1, &bond_callback_queue_)
  {
    load_server_ = nh_.advertiseService("load_nodelet", &LoaderROS::serviceLoad, this);
    unload_server_ = nh_.advertiseService("unload_nodelet", &LoaderROS::serviceUnload, this);
    list_server_ = nh_.advertiseService("list", &LoaderROS::serviceList, this);

    bond_spinner_.start();
  }

private:
  bool serviceLoad(nodelet::NodeletLoad::Request &req,
                   nodelet::NodeletLoad::Response &res)
  {
    // build map
    M_string remappings;
    if (req.remap_source_args.size() != req.remap_target_args.size())
    {
      ROS_ERROR("Bad remapppings provided, target and source of different length");
    }
    else
    {
      for (size_t i = 0; i < req.remap_source_args.size(); ++i)
      {
        remappings[ros::names::resolve(req.remap_source_args[i])] = ros::names::resolve(req.remap_target_args[i]);
        ROS_DEBUG("%s:%s\n", ros::names::resolve(req.remap_source_args[i]).c_str(), remappings[ros::names::resolve(req.remap_source_args[i])].c_str());
      }
    }

    boost::shared_ptr<bond::Bond> bond;
    if (!req.bond_id.empty())
    {
      bond.reset(new bond::Bond(nh_.getNamespace() + "/bond", req.bond_id));
      bond->setCallbackQueue(&bond_callback_queue_);
    }

    res.success = parent_->load(req.name, req.type, remappings, req.my_argv, bond);
    if (bond)
      bond->start();
    return res.success;
  }

  bool serviceUnload(nodelet::NodeletUnload::Request &req,
                     nodelet::NodeletUnload::Response &res)
  {
    res.success = parent_->unload(req.name);
    if (!res.success)
    {
      ROS_ERROR("Failed to find nodelet with name '%s' to unload.", req.name.c_str());
    }

    return res.success;
  }

  bool serviceList(nodelet::NodeletList::Request &req,
                   nodelet::NodeletList::Response &res)
  {

    res.nodelets = parent_->listLoadedNodelets();
    return true;
  }

  Loader* parent_;
  ros::NodeHandle nh_;
  ros::ServiceServer load_server_;
  ros::ServiceServer unload_server_;
  ros::ServiceServer list_server_;

  
  ros::CallbackQueue bond_callback_queue_;
  ros::AsyncSpinner bond_spinner_;
};
} // namespace detail

Loader::Loader(bool provide_ros_api)
: loader_(new pluginlib::ClassLoader<Nodelet>("nodelet", "nodelet::Nodelet"))
{
  constructorImplementation(provide_ros_api, ros::NodeHandle("~"));
}

Loader::Loader(ros::NodeHandle server_nh)
  : loader_(new pluginlib::ClassLoader<Nodelet>("nodelet", "nodelet::Nodelet"))
{
  constructorImplementation(true, server_nh);
}

void Loader::constructorImplementation(bool provide_ros_api, ros::NodeHandle server_nh)
{
  if (provide_ros_api)
  {
    services_.reset(new detail::LoaderROS(this, server_nh));
    int num_threads_param;
    if (server_nh.getParam ("num_worker_threads", num_threads_param))
    {
      callback_manager_ = detail::CallbackQueueManagerPtr (new detail::CallbackQueueManager (num_threads_param));
      ROS_INFO("Initializing nodelet with %d worker threads.", num_threads_param);
    }
  }
  if (!callback_manager_)
    callback_manager_ = detail::CallbackQueueManagerPtr (new detail::CallbackQueueManager);
}

Loader::~Loader()
{
  services_.reset();

  // About the awkward ordering here:
  // We have to make callback_manager_ flush all callbacks and stop the worker threads BEFORE
  // destroying the nodelets. Otherwise the worker threads may act on nodelet data as/after
  // it's destroyed. But we have to destroy callback_manager_ after the nodelets, because the
  // nodelet destructor tries to remove its queues from the callback manager.
  callback_manager_->stop();
  nodelets_.clear();
  callback_manager_.reset();
}

bool Loader::load(const std::string &name, const std::string& type, const ros::M_string& remappings, const std::vector<std::string> & my_argv, boost::shared_ptr<bond::Bond> bond)
{
  boost::mutex::scoped_lock lock (lock_);
  if (nodelets_.count(name) > 0)
  {
    ROS_ERROR("Cannot load nodelet %s for one exists with that name already", name.c_str());
    return false;
  }

  //\TODO store type in string format too, or provide accessors from pluginlib
  try
  {
    NodeletPtr p(loader_->createClassInstance(type));
    if (!p)
    {
      return false;
    }

    nodelets_[name] = p;
    ROS_DEBUG("Done loading nodelet %s", name.c_str());

    /// @todo Pass callback queues to p->init directly
    p->st_callback_queue_.reset(new detail::CallbackQueue(callback_manager_.get(), p));
    p->mt_callback_queue_.reset(new detail::CallbackQueue(callback_manager_.get(), p));
    p->init(name, remappings, my_argv, callback_manager_.get(), bond);

    if (bond)
      bond->setBrokenCallback(boost::bind(&Loader::unload, this, name));

    ROS_DEBUG("Done initing nodelet %s", name.c_str());
    return true;
  }
  catch (pluginlib::PluginlibException& e)
  {
    ROS_ERROR("Failed to load nodelet [%s] of type [%s]: %s", name.c_str(), type.c_str(), e.what());
  }

  return false;
}

bool Loader::unload (const std::string & name)
{
  boost::mutex::scoped_lock lock (lock_);
  M_stringToNodelet::iterator it = nodelets_.find (name);
  if (it != nodelets_.end ())
  {
    it->second->disable();
    nodelets_.erase (it);
    ROS_DEBUG ("Done unloading nodelet %s", name.c_str ());
    return (true);
  }

  return (false);
}

/** \brief Clear all nodelets from this chain */
bool Loader::clear ()
{
  /// @todo This isn't really safe - can result in worker threads for outstanding callbacks
  /// operating on nodelet data as/after it's destroyed.
  boost::mutex::scoped_lock lock (lock_);
  nodelets_.clear ();
  return (true);
};

/**\brief List the names of all loaded nodelets */
std::vector<std::string> Loader::listLoadedNodelets()
{
  boost::mutex::scoped_lock lock (lock_);
  std::vector<std::string> output;
  std::map< std::string, boost::shared_ptr<Nodelet> >::iterator it = nodelets_.begin();
  for (; it != nodelets_.end(); ++it)
  {
    output.push_back(it->first);
  }
  return output;
}

} // namespace nodelet


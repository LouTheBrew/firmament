// The Firmament project
// Copyright (c) 2011-2012 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// Platform-independent coordinator class implementation. This is subclassed by
// the platform-specific coordinator classes.

#include "engine/coordinator.h"

#include <string>
#include <utility>

#ifdef __PLATFORM_HAS_BOOST__
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#endif

#include "base/resource_desc.pb.h"
#include "base/resource_topology_node_desc.pb.h"
#include "storage/simple_object_store.h"
#include "messages/base_message.pb.h"
#include "misc/protobuf_envelope.h"
#include "misc/map-util.h"
#include "misc/utils.h"
#include "messages/storage_registration_message.pb.h"
#include "messages/storage_message.pb.h"


// It is necessary to declare listen_uri here, since "node.o" comes after
// "coordinator.o" in linking order (I *think*).
DECLARE_string(listen_uri);
DEFINE_string(parent_uri, "", "The URI of the parent coordinator to register "
        "with.");
#ifdef __HTTP_UI__
DEFINE_bool(http_ui, true, "Enable HTTP interface");
DEFINE_int32(http_ui_port, 8080,
        "The port that the HTTP UI will be served on; -1 to disable.");
#endif

namespace firmament {

Coordinator::Coordinator(PlatformID platform_id)
  : Node(platform_id, GenerateUUID()),
    associated_resources_(new ResourceMap_t),
    local_resource_topology_(new ResourceTopologyNodeDescriptor),
    job_table_(new JobMap_t),
    task_table_(new TaskMap_t),
    topology_manager_(new TopologyManager()),
    object_store_(new store::SimpleObjectStore(uuid_)),
    scheduler_(new SimpleScheduler(job_table_, associated_resources_,
                                   object_store_, task_table_,
                                   topology_manager_,
                                   m_adapter_,
                                   uuid_,
                                   FLAGS_listen_uri)) {
  // Start up a coordinator according to the platform parameter
  string hostname = boost::asio::ip::host_name();
  string desc_name = "Coordinator on " + hostname;
  resource_desc_.set_uuid(to_string(uuid_));
  resource_desc_.set_friendly_name(desc_name);
  resource_desc_.set_type(ResourceDescriptor::RESOURCE_MACHINE);
  resource_desc_.set_storage_engine(object_store_->get_listening_interface());
  local_resource_topology_->mutable_resource_desc()->CopyFrom(
      resource_desc_);
  // Log information
  LOG(INFO) << "Coordinator starting on host " << FLAGS_listen_uri
          << ", platform " << platform_id << ", uuid " << uuid_;
  LOG(INFO) << "Storage Engine is listening on interface : "
            << object_store_->get_listening_interface();
  switch (platform_id) {
      case PL_UNIX:
      {
          break;
      }
      default:
          LOG(FATAL) << "Unimplemented!";
  }
}

Coordinator::~Coordinator() {
  // TODO(malte): check destruction order in C++; c_http_ui_ may already
  // have been destructed when we get here.
  /*#ifdef __HTTP_UI__
    if (FLAGS_http_ui && c_http_ui_)
      c_http_ui_->Shutdown(false);
  #endif*/
}

bool Coordinator::RegisterWithCoordinator(
    StreamSocketsChannel<BaseMessage>* chan) {
  BaseMessage bm;
  ResourceDescriptor* rd = bm.MutableExtension(
          register_extn)->mutable_res_desc();
  rd->CopyFrom(resource_desc_); // copies current local RD!
  ResourceTopologyNodeDescriptor* rtnd = bm.MutableExtension(
          register_extn)->mutable_rtn_desc();
  rtnd->CopyFrom(*local_resource_topology_);
  bm.MutableExtension(register_extn)->set_uuid(
          boost::uuids::to_string(uuid_));
  bm.MutableExtension(register_extn)->set_location(
          chan->LocalEndpointString());
  // wrap in envelope
  VLOG(2) << "Sending registration message...";
  // send heartbeat message
  return SendMessageToRemote(chan, &bm);
}

void Coordinator::DetectLocalResources() {
  // Inform the user about the number of local PUs.
  uint64_t num_local_pus = topology_manager_->NumProcessingUnits();
  LOG(INFO) << "Found " << num_local_pus << " local PUs.";
  LOG(INFO) << "Resource URI is " << node_uri_;
  // Get local resource topology and save it to the topology protobuf
  // TODO(malte): Figure out how this interacts with dynamically added
  // resources; currently, we only run detection (i.e. the DetectLocalResources
  // method) once at startup.
  ResourceTopologyNodeDescriptor* root_node =
      local_resource_topology_->add_children();
  topology_manager_->AsProtobuf(root_node);
  local_resource_topology_->set_parent_id(to_string(uuid_));
  topology_manager_->TraverseProtobufTree(
      local_resource_topology_,
      boost::bind(&Coordinator::AddResource, this, _1, node_uri_, true));
}

void Coordinator::AddResource(ResourceDescriptor* resource_desc,
                              const string& endpoint_uri,
                              bool local) {
  CHECK(resource_desc);
  // Compute resource ID
  ResourceID_t res_id = ResourceIDFromString(resource_desc->uuid());
  // Add resource to local resource set
  VLOG(1) << "Adding resource " << res_id << " to resource map; "
          << "endpoint URI is " << endpoint_uri;
  CHECK(InsertIfNotPresent(associated_resources_.get(), res_id,
          new ResourceStatus(resource_desc, endpoint_uri,
                             GetCurrentTimestamp())));
  // Register with scheduler if this resource is schedulable
  if (resource_desc->type() == ResourceDescriptor::RESOURCE_PU) {
    // TODO(malte): We make the assumption here that any local PU resource is
    // exclusively owned by this coordinator, and set its state to IDLE if it is
    // currently unknown. If coordinators were to ever shared PUs, we'd need
    // something more clever here.
    resource_desc->set_schedulable(true);
    if (resource_desc->state() == ResourceDescriptor::RESOURCE_UNKNOWN)
      resource_desc->set_state(ResourceDescriptor::RESOURCE_IDLE);
    scheduler_->RegisterResource(res_id, local);
    VLOG(1) << "Added " << (local ? "local" : "remote") << " resource "
            << resource_desc->uuid()
            << " [" << resource_desc->friendly_name()
            << "] to scheduler.";
  }
}

void Coordinator::Run() {
  // Test topology detection
  LOG(INFO) << "Detecting resource topology:";
  topology_manager_->DebugPrintRawTopology();
  DetectLocalResources();

  // Coordinator starting -- set up and wait for workers to connect.
  m_adapter_->ListenURI(FLAGS_listen_uri);
  m_adapter_->RegisterAsyncMessageReceiptCallback(
          boost::bind(&Coordinator::HandleIncomingMessage, this, _1));
  m_adapter_->RegisterAsyncErrorPathCallback(
          boost::bind(&Coordinator::HandleIncomingReceiveError, this,
          boost::asio::placeholders::error, _2));

#ifdef __HTTP_UI__
  InitHTTPUI();
#endif

  // Do we have a parent? If so, register with it now.
  if (FLAGS_parent_uri != "") {
      StreamSocketsChannel<BaseMessage>* chan =
          new StreamSocketsChannel<BaseMessage > (
              StreamSocketsChannel<BaseMessage>::SS_TCP);
      CHECK(ConnectToRemote(FLAGS_parent_uri, chan))
              << "Failed to connect to parent!";
      RegisterWithCoordinator(chan);
      InformStorageEngineNewResource(&resource_desc_);
  }

  // Main loop
  while (!exit_) {
      // Wait for events (i.e. messages from workers.
      // TODO(malte): we need to think about any actions that the coordinator
      // itself might need to take, and how they can be triggered
      VLOG(3) << "Hello from main loop!";
      AwaitNextMessage();
  }

  // We have dropped out of the main loop and are exiting
  // TODO(malte): any cleanup we need to do; hand-over to another coordinator if
  // possible?
  Shutdown("dropped out of main loop");
}

const JobDescriptor* Coordinator::DescriptorForJob(const string& job_id) {
  JobID_t job_uuid = JobIDFromString(job_id);
  JobDescriptor *jd = FindOrNull(*job_table_, job_uuid);
  return jd;
}

void Coordinator::HandleIncomingMessage(BaseMessage *bm) {
  uint32_t handled_extensions = 0;
  // Registration message
  if (bm->HasExtension(register_extn)) {
    const RegistrationMessage& msg = bm->GetExtension(register_extn);
    HandleRegistrationRequest(msg);
    handled_extensions++;
  }
  // Resource Heartbeat message
  if (bm->HasExtension(heartbeat_extn)) {
    const HeartbeatMessage& msg = bm->GetExtension(heartbeat_extn);
    HandleHeartbeat(msg);
    handled_extensions++;
  }
  // Task heartbeat message
  if (bm->HasExtension(task_heartbeat_extn)) {
    const TaskHeartbeatMessage& msg = bm->GetExtension(task_heartbeat_extn);
    HandleTaskHeartbeat(msg);
    handled_extensions++;
  }
  // Task state change message
  if (bm->HasExtension(task_state_extn)) {
    const TaskStateMessage& msg = bm->GetExtension(task_state_extn);
    HandleTaskStateChange(msg);
    handled_extensions++;
  }
  // Task spawn message
  if (bm->HasExtension(task_spawn_extn)) {
    const TaskSpawnMessage& msg = bm->GetExtension(task_spawn_extn);
    HandleTaskSpawn(msg);
    handled_extensions++;
  }
  // Task info request message
  if (bm->HasExtension(task_info_req_extn)) {
    const TaskInfoRequestMessage& msg = bm->GetExtension(task_info_req_extn);
    HandleTaskInfoRequest(msg);
    handled_extensions++;
  }
  // Storage engine registration
  if (bm->HasExtension(register_storage_extn)) {
    const StorageRegistrationMessage& msg =
        bm->GetExtension(register_storage_extn);
    HandleStorageRegistrationRequest(msg);
    handled_extensions++;
  }
  // Storage engine discovery
  if (bm->HasExtension(storage_discover_message_extn)) {
    const StorageDiscoverMessage& msg = bm->GetExtension(
        storage_discover_message_extn);
    HandleStorageDiscoverRequest(msg);
    handled_extensions++;
  }
  // Task delegation message
 if (bm->HasExtension(task_delegation_extn)) {
    const TaskDelegationMessage& msg = bm->GetExtension(
        task_delegation_extn);
    HandleTaskDelegationRequest(msg);
    handled_extensions++;
  }
  // Check that we have handled at least one sub-message
  if (handled_extensions == 0)
    LOG(ERROR) << "Ignored incoming message, no known extension present, "
               << "so cannot handle it: " << bm->DebugString();
}

void Coordinator::HandleHeartbeat(const HeartbeatMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  ResourceStatus** rsp = FindOrNull(*associated_resources_, uuid);
  if (!rsp) {
      LOG(WARNING) << "HEARTBEAT from UNKNOWN resource (uuid: "
              << msg.uuid() << ")!";
  } else {
      LOG(INFO) << "HEARTBEAT from resource " << msg.uuid()
                << " (last seen at " << (*rsp)->last_heartbeat() << ")";
      // Update timestamp
      (*rsp)->set_last_heartbeat(GetCurrentTimestamp());
  }
}

void Coordinator::HandleRegistrationRequest(
    const RegistrationMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  ResourceStatus** rdp = FindOrNull(*associated_resources_, uuid);
  if (!rdp) {
    LOG(INFO) << "REGISTERING NEW RESOURCE (uuid: " << msg.uuid() << ")";
    // N.B.: below creates a new resource descriptor
    ResourceDescriptor* rd = new ResourceDescriptor(msg.res_desc());
    // Insert the root of the registered topology into the topology tree
    ResourceTopologyNodeDescriptor* rtnd =
        local_resource_topology_->add_children();
    rtnd->CopyFrom(msg.rtn_desc());
    rtnd->set_parent_id(resource_desc_.uuid());
    // Recursively add its child resources to resource map and topology tree
    topology_manager_->TraverseProtobufTree(
        rtnd, boost::bind(&Coordinator::AddResource, this, _1,
                          msg.location(), false));
    InformStorageEngineNewResource(rd);
  } else {
    LOG(INFO) << "REGISTRATION request from resource " << msg.uuid()
              << " that we already know about. "
              << "Checking if this is a recovery.";
    // TODO(malte): Implement checking logic, deal with recovery case
    // Update timestamp (registration request is an implicit heartbeat)
    (*rdp)->set_last_heartbeat(GetCurrentTimestamp());
  }
}

void Coordinator::HandleTaskHeartbeat(const TaskHeartbeatMessage& msg) {
  TaskID_t task_id = msg.task_id();
  TaskDescriptor** tdp = FindOrNull(*task_table_, task_id);
  if (!tdp) {
    LOG(WARNING) << "HEARTBEAT from UNKNOWN task (ID: "
                 << task_id << ")!";
  } else {
    LOG(INFO) << "HEARTBEAT from task " << task_id;
    // Process the profiling information submitted by the task, add it to
    // the knowledge base
    // TODO(malte): implement this
  }
}

void Coordinator::HandleTaskDelegationRequest(
    const TaskDelegationMessage& msg) {
  VLOG(1) << "Handling requested delegation of task "
          << msg.task_descriptor().uid() << " from resource "
          << msg.delegating_resource_id();
  // Check if there is room for this task here
  // (or maybe enqueue it?)
  // Return ACK/NACK
}

void Coordinator::HandleTaskInfoRequest(const TaskInfoRequestMessage& msg) {
  // Send response: the task descriptor if the task is known to this
  // coordinator
  VLOG(1) << "Resource " << msg.requesting_resource_id()
          << " requests task information for " << msg.task_id();
  TaskDescriptor** task_desc_ptr = FindOrNull(*task_table_, msg.task_id());
  CHECK_NOTNULL(task_desc_ptr);
  BaseMessage resp;
  // XXX(malte): ugly hack!
  SUBMSG_WRITE(resp, task_info_resp, task_id, msg.task_id());
  resp.MutableExtension(task_info_resp_extn)->
      mutable_task_desc()->CopyFrom(**task_desc_ptr);
  m_adapter_->SendMessageToEndpoint(msg.requesting_endpoint(), resp);
}

void Coordinator::HandleTaskSpawn(const TaskSpawnMessage& msg) {
  VLOG(1) << "Handling task spawn for new task "
          << msg.spawned_task_desc().uid() << ", child of "
          << msg.creating_task_id();
  // Get the descriptor for the spawning task
  TaskDescriptor** spawner;
  CHECK(spawner = FindOrNull(*task_table_, msg.creating_task_id()));
  // Extract new task descriptor from the message received
  //TaskDescriptor* spawnee = new TaskDescriptor;
  TaskDescriptor* spawnee = (*spawner)->add_spawned();
  spawnee->CopyFrom(msg.spawned_task_desc());
  InsertIfNotPresent(task_table_.get(), spawnee->uid(), spawnee);
  // Extract job ID (we expect it to be set)
  CHECK(msg.spawned_task_desc().has_job_id());
  JobID_t job_id = JobIDFromString(msg.spawned_task_desc().job_id());
  // Find task graph for job
  TaskGraph** task_graph_pptr = FindOrNull(task_graph_table_, job_id);
  CHECK_NOTNULL(task_graph_pptr);
  TaskGraph* task_graph_ptr = *task_graph_pptr;
  VLOG(1) << "Task graph is at " << task_graph_ptr;
  // Add task to task graph
  //task_graph_ptr->AddChildTask(spawner, spawnee)
  // Run the scheduler for this job
  JobDescriptor* job = FindOrNull(*job_table_, job_id);
  CHECK_NOTNULL(job);
  scheduler_->ScheduleJob(job);
}

void Coordinator::HandleTaskStateChange(
    const TaskStateMessage& msg) {
  switch (msg.new_state()) {
    case TaskDescriptor::COMPLETED:
    {
      VLOG(1) << "Task " << msg.id() << " now in state COMPLETED.";
      // XXX(malte): tear down the respective connection, cleanup
      TaskDescriptor** td_ptr = FindOrNull(*task_table_, msg.id());
      CHECK(td_ptr) << "Received completion message for unknown task "
              << msg.id();
      (*td_ptr)->set_state(TaskDescriptor::COMPLETED);
      scheduler_->HandleTaskCompletion(*td_ptr);
      break;
    }
    default:
      VLOG(1) << "Task " << msg.id() << "'s state changed to "
              << static_cast<uint64_t> (msg.new_state());
      break;
  }
}

#ifdef __HTTP_UI__
void Coordinator::InitHTTPUI() {
  // Start up HTTP interface
  if (FLAGS_http_ui && FLAGS_http_ui_port > 0) {
    // TODO(malte): This is a hack to avoid failure of shared_from_this()
    // because we do not have a shared_ptr to this object yet. Not sure if this
    // is safe, though.... (I think it is, as long as the Coordinator's main()
    // still holds a shared_ptr to the Coordinator).
    //shared_ptr<Coordinator> dummy(this);
    c_http_ui_.reset(new CoordinatorHTTPUI(shared_from_this()));
    c_http_ui_->Init(FLAGS_http_ui_port);
  }
}
#endif

void Coordinator::AddJobsTasksToTables(TaskDescriptor* td, JobID_t job_id) {
  // Set job ID field on task. We do this here since we've only just generated
  // the job ID in the job submission, which passes it in.
  td->set_job_id(to_string(job_id));
  // Insert task into task table
  VLOG(1) << "Adding task " << td->uid() << " to task table.";
  if (!InsertIfNotPresent(task_table_.get(), td->uid(), td)) {
    VLOG(1) << "Task " << td->uid() << " already exists in "
            << "task table, so not adding it again.";
  }
  // Adds its outputs to the object table and generate future references for
  // them.
  for (RepeatedPtrField<ReferenceDescriptor>::iterator output_iter =
       td->mutable_outputs()->begin();
       output_iter != td->mutable_outputs()->end();
       ++output_iter) {
    // First set the producing task field on the task outputs
    output_iter->set_producing_task(td->uid());
    VLOG(1) << "Considering task output " << output_iter->id() << ", "
            << "adding to local object table";
    if (object_store_->addReference(output_iter->id(), &(*output_iter))) {
      VLOG(1) << "Output " << output_iter->id() << " already exists in "
              << "local object table. Not adding again.";
    }
    // Check that the object was actually stored
    if (object_store_->GetReference(output_iter->id()) != NULL)
      VLOG(3) << "Object is indeed in object store";
    else
      VLOG(3) << "Error: Object is not in object store";
  }
  // Process children recursively
  for (RepeatedPtrField<TaskDescriptor>::iterator task_iter =
       td->mutable_spawned()->begin();
       task_iter != td->mutable_spawned()->end();
       ++task_iter) {
    AddJobsTasksToTables(&(*task_iter), job_id);
  }
}

const string Coordinator::SubmitJob(const JobDescriptor& job_descriptor) {
  // Generate a job ID
  // TODO(malte): This should become deterministic, and based on the
  // inputs/outputs somehow, maybe.
  JobID_t new_job_id = GenerateJobID();
  LOG(INFO) << "NEW JOB: " << new_job_id;
  VLOG(2) << "Details:\n" << job_descriptor.DebugString();
  // Clone the JD and update it with some information
  JobDescriptor* new_jd = new JobDescriptor;
  new_jd->CopyFrom(job_descriptor);
  new_jd->set_uuid(to_string(new_job_id));
  // Set the root task ID (which is 0 or unset on submission)
  new_jd->mutable_root_task()->set_uid(GenerateRootTaskID(*new_jd));
  // Add job to local job table
  CHECK(InsertIfNotPresent(job_table_.get(), new_job_id, *new_jd));
  // Create a dynamic task graph for the job
  TaskGraph* new_dtg = new TaskGraph(new_jd->mutable_root_task());
  // Store the task graph
  InsertIfNotPresent(&task_graph_table_, new_job_id, new_dtg);
  // Add itself and its spawned tasks (if any) to the relevant tables:
  // - tasks to the task_table_
  // - inputs/outputs to the object_table_
  // and set the job_id field on every task.
  AddJobsTasksToTables(new_jd->mutable_root_task(), new_job_id);
  // Set up job outputs
  for (RepeatedField<DataObjectID_t>::const_iterator output_iter =
       new_jd->output_ids().begin();
       output_iter != new_jd->output_ids().end();
       ++output_iter) {
    VLOG(1) << "Considering job output " << *output_iter;
    // The root task must produce all of the non-existent job outputs, so they
    // should all be in the object table now.
    CHECK(object_store_->GetReference(*output_iter));
  }
#ifdef __SIMULATE_SYNTHETIC_DTG__
  LOG(INFO) << "SIMULATION MODE -- generating synthetic task graph!";
  sim_dtg_generator_.reset(new sim::SimpleDTGGenerator(FindOrNull(*job_table_,
                                                                  new_job_id)));
  boost::thread t(boost::bind(&sim::SimpleDTGGenerator::Run,
                              sim_dtg_generator_));
#endif
  // Kick off the scheduler for this job.
  uint64_t num_scheduled = scheduler_->ScheduleJob(
      FindOrNull(*job_table_, new_job_id));
  VLOG(1) << "Attempted to schedule job " << new_job_id << ", successfully "
          << "scheduled " << num_scheduled << " tasks.";
  // Finally, return the new job's ID
  return to_string(new_job_id);
}

void Coordinator::Shutdown(const string& reason) {
  LOG(INFO) << "Coordinator shutting down; reason: " << reason;
#ifdef __HTTP_UI__
  if (FLAGS_http_ui && c_http_ui_ && c_http_ui_->active())
      c_http_ui_->Shutdown(false);
#endif
  m_adapter_->StopListen();
  VLOG(1) << "All connections shut down; now exiting...";
  // Toggling the exit flag will make the Coordinator drop out of its main loop.
  exit_ = true;
}

/* Storage Engine Interface - The storage engine is notified of the location
 * of new storage engines in one of three ways:
 * 1) if a new resource is added to the coordinator, then all the local
 * storage engines (which share the same coordinator) should be informed.
 * Automatically add this to the list of peers
 * 2) if receive a StorageRegistrationRequest, this signals the desire of a
 * storage engine to be registered with (either) all of the storage engines
 * managed by this coordinator, or its local engine. StorageRegistrationRequests
 * can also arrive directly to the storage engine. The coordinator supports this
 * because outside resources/coordinators may not know the whole topology within
 * the coordinator. I *suspect* this technique will be faster
 * TODO(tach): improve efficiency */
void Coordinator::InformStorageEngineNewResource(ResourceDescriptor* rd_new) {
  VLOG(2) << "Inform Storage Engine of New Resources ";

  /* Create Storage Registration Message */
  BaseMessage base;
  StorageRegistrationMessage* message = new StorageRegistrationMessage();
  message->set_peer(true);
  CHECK_NE(rd_new->storage_engine(), "")
    << "Storage engine URI missing on resource " << rd_new->uuid();
  message->set_storage_interface(rd_new->storage_engine());
  message->set_uuid(rd_new->uuid());

  StorageRegistrationMessage& message_ref = *message;

  vector<ResourceStatus*> rs_vec = associated_resources();

  /* Handle Local Engine First */

  object_store_->HandleStorageRegistrationRequest(message_ref);

  /* Handle other resources' engines*/
  for (vector<ResourceStatus*>::iterator it = rs_vec.begin();
       it != rs_vec.end();
       ++it) {
    ResourceStatus* rs = *it;
    // Only machine-level resources can have a storage engine
    // TODO(malte): is this a correct assumption? We could have per-core
    // storage engines.
    if (rs->descriptor().type() != ResourceDescriptor::RESOURCE_MACHINE)
      continue;
    if (rs->descriptor().storage_engine() != "")
      VLOG(2) << "Resource " << rs->descriptor().uuid() << " does not have a "
              << "storage engine URI set; skipping notification!";
      continue;
    const string& uri = rs->descriptor().storage_engine();
    CHECK_NE(uri, "") << "Missing storage engine URI on RD for "
                      << rs->descriptor().uuid();
    if (!m_adapter_->GetChannelForEndpoint(uri)) {
      StreamSocketsChannel<BaseMessage>* chan =
          new StreamSocketsChannel<BaseMessage > (
              StreamSocketsChannel<BaseMessage>::SS_TCP);
      Coordinator::ConnectToRemote(uri, chan);
    } else {
      m_adapter_->SendMessageToEndpoint(uri, (BaseMessage&)message_ref);
    }
  }
}

/* Wrapper Method - Forward to Object Store. This method is useful
 * for other storage engines which simply talk to the coordinator,
 * letting it to the broadcast to its resources
 */
void Coordinator::HandleStorageRegistrationRequest(
        const StorageRegistrationMessage& msg) {
  if (object_store_ == NULL) { // In theory, each node should have an object
                               // store which has already been instantiated. So
                               // this case should never happen.
      VLOG(1) << "No object store detected for this node. Storage Registration"
              << "Message discarded";
  } else {
      object_store_->HandleStorageRegistrationRequest(msg);
  }
}

/* This is a message sent by TaskLib which seeks to discover
 where the storage engine is - this is only important if the
 storage engine is not guaranteed to be local */
void Coordinator::HandleStorageDiscoverRequest(
    const StorageDiscoverMessage& msg) {
  ResourceID_t uuid = ResourceIDFromString(msg.uuid());
  ResourceStatus** rsp = FindOrNull(*associated_resources_, uuid);
  ResourceDescriptor* rd = (*rsp)->mutable_descriptor();
  const string& uri = rd->storage_engine();
  StorageDiscoverMessage* reply = new StorageDiscoverMessage();
  reply->set_uuid(msg.uuid());
  reply->set_uri(msg.uri());
  reply->set_storage_uri(uri);
  /* Send Message*/
}

} // namespace firmament


#include <cstdint>
#include "Scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std;

// =============================================================================
// Configuration
// =============================================================================

static const double ENERGY_WEIGHT = 0.60;
static const double SLA_WEIGHT = 0.40;

static const double MAX_UTILIZATION = 0.98;

static const double SLA0_THRESHOLD = 0.70;
static const double SLA1_THRESHOLD = 0.80;
static const double SLA2_THRESHOLD = 0.85;
static const double SLA3_THRESHOLD = 0.90;

// =============================================================================
// Cached machine information
// =============================================================================

// Machine specifications are read once during initialization.
// Dynamic state, memory, VM count, and demand are maintained locally.

static vector<MachineInfo_t> g_machine_info;
static vector<MachineState_t> g_machine_state;

static vector<unsigned> g_machine_memory;
static vector<unsigned> g_machine_active_vms;
static vector<VMId_t> g_machine_vm;
static vector<unsigned> g_machine_active_tasks;
static vector<bool> g_machine_has_vm;
static vector<bool> g_machine_vm_reserved;
static vector<VMType_t> g_machine_vm_type;

static vector<double> g_machine_demand;

// =============================================================================
// Task bookkeeping
// =============================================================================

// Each task has exactly one VM.

static map<TaskId_t, VMId_t> g_task_vm;
static map<TaskId_t, MachineId_t> g_task_machine;

static map<TaskId_t, double> g_task_demand;
static map<TaskId_t, unsigned> g_task_memory;

// Tasks waiting for a machine to wake up.
static map<MachineId_t, set<TaskId_t>> g_pending_tasks;
static map<TaskId_t, MachineId_t> g_task_pending_machine;

// Prevent duplicate machine state transitions.
static set<MachineId_t> g_waking_machines;
static set<MachineId_t> g_powering_down_machines;

// =============================================================================
// Task demand estimation
// =============================================================================

// Fixed average processing demand:
//
// D_i = total_instructions / (target_completion - arrival)
//
// Since simulator time is in microseconds, this gives instructions/us,
// equivalent to MIPS.

static double GetTaskDemand(TaskId_t task_id)
{
    TaskInfo_t task = GetTaskInfo(task_id);

    if (task.total_instructions == 0)
        return 0.0;

    if (task.target_completion <= task.arrival)
        return numeric_limits<double>::max() / 1000.0;

    double duration =
        static_cast<double>(
            task.target_completion - task.arrival);

    return static_cast<double>(task.total_instructions) / duration;
}

// =============================================================================
// Machine calculations
// =============================================================================

static double GetMachineCapacity(MachineId_t machine_id)
{
    const MachineInfo_t& machine =
        g_machine_info[machine_id];

    if (machine.num_cpus == 0 ||
        machine.performance.size() <= P0) {
        return 0.0;
    }

    return static_cast<double>(machine.num_cpus)
           * machine.performance[P0];
}

static double GetMachineUtilization(MachineId_t machine_id)
{
    double capacity = GetMachineCapacity(machine_id);

    if (capacity <= 0.0)
        return numeric_limits<double>::max();

    return g_machine_demand[machine_id] / capacity;
}

static double GetTaskUtilization(
    TaskId_t task_id,
    MachineId_t machine_id)
{
    double capacity = GetMachineCapacity(machine_id);

    if (capacity <= 0.0)
        return numeric_limits<double>::max();

    return g_task_demand[task_id] / capacity;
}

static double GetSLALimit(SLAType_t sla)
{
    switch (sla) {
        case SLA0: return SLA0_THRESHOLD;
        case SLA1: return SLA1_THRESHOLD;
        case SLA2: return SLA2_THRESHOLD;
        case SLA3: return SLA3_THRESHOLD;
        default:   return SLA3_THRESHOLD;
    }
}

// =============================================================================
// Compatibility and feasibility
// =============================================================================

static bool IsCompatible(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    const MachineInfo_t& machine =
        g_machine_info[machine_id];

    if (machine.cpu != RequiredCPUType(task_id))
        return false;

    if (IsTaskGPUCapable(task_id) && !machine.gpus)
        return false;

    // A shared VM can only host tasks requiring the same guest OS type.
    if (g_machine_has_vm[machine_id] &&
        g_machine_vm_type[machine_id] != RequiredVMType(task_id))
        return false;

    return true;
}

static bool HasEnoughMemory(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    unsigned required = GetTaskMemory(task_id);
    if (!g_machine_vm_reserved[machine_id])
        required += VM_MEMORY_OVERHEAD;

    if (g_machine_memory[machine_id] >
        g_machine_info[machine_id].memory_size) {
        return false;
    }

    unsigned available =
        g_machine_info[machine_id].memory_size
        - g_machine_memory[machine_id];

    return available >= required;
}

static bool CanHostTask(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    if (!IsCompatible(machine_id, task_id))
        return false;

    if (!HasEnoughMemory(machine_id, task_id))
        return false;

    return true;
}

// =============================================================================
// Utility function
// =============================================================================

// Lower utility cost is better.
//
// Energy component:
//   Estimated incremental CPU power plus a machine activation penalty.
//
// SLA component:
//   Penalty for projected utilization above the incoming task's SLA limit.

static double GetPlacementUtility(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    const MachineInfo_t& machine =
        g_machine_info[machine_id];

    double capacity = GetMachineCapacity(machine_id);

    if (capacity <= 0.0)
        return numeric_limits<double>::max();

    double current_utilization =
        GetMachineUtilization(machine_id);

    double task_utilization =
        GetTaskUtilization(task_id, machine_id);

    double projected_utilization =
        current_utilization + task_utilization;

    // -------------------------------------------------------------------------
    // Energy cost
    // -------------------------------------------------------------------------

    double p0_power = 0.0;

    if (machine.p_states.size() > P0) {
        p0_power = machine.p_states[P0];
    }

    double incremental_cpu_power =
        p0_power * machine.num_cpus * task_utilization;

    double activation_power = 0.0;

    MachineState_t state = g_machine_state[machine_id];

    if (state != S0 && machine.s_states.size() > S0) {
        double active_power = machine.s_states[S0];

        double current_power = 0.0;

        if (machine.s_states.size() >
            static_cast<unsigned>(state)) {
            current_power = machine.s_states[state];
        }

        activation_power =
            max(0.0, active_power - current_power);
    }

    // A sleeping machine must support the existing reserved demand too.
    if (state != S0) {
        incremental_cpu_power =
            p0_power * machine.num_cpus
            * projected_utilization;
    }

    double estimated_energy_cost =
        incremental_cpu_power + activation_power;

    double normalization_power = 1.0;

    if (machine.s_states.size() > S0) {
        normalization_power =
            max(1.0, static_cast<double>(machine.s_states[S0]));
    }

    double energy_score =
        estimated_energy_cost / normalization_power;

    // -------------------------------------------------------------------------
    // SLA risk
    // -------------------------------------------------------------------------

    double sla_limit =
        GetSLALimit(RequiredSLA(task_id));

    double sla_risk = 0.0;

    if (projected_utilization > sla_limit) {
        double overage =
            (projected_utilization - sla_limit)
            / (1.0 - sla_limit);

        sla_risk = overage * overage;
    }

    return ENERGY_WEIGHT * energy_score
           + SLA_WEIGHT * sla_risk;
}

// =============================================================================
// Best-machine selection
// =============================================================================

static bool FindBestMachine(
    TaskId_t task_id,
    MachineId_t& selected_machine)
{
    bool found = false;

    double best_utility =
        numeric_limits<double>::max();

    for (MachineId_t machine_id = 0;
         machine_id < g_machine_info.size();
         ++machine_id) {

        // Do not allocate to a machine currently being powered down.
        if (g_powering_down_machines.count(machine_id))
            continue;

        if (!CanHostTask(machine_id, task_id))
            continue;

        double utility =
            GetPlacementUtility(machine_id, task_id);

        if (!found || utility < best_utility) {
            found = true;
            best_utility = utility;
            selected_machine = machine_id;
        }
    }

    return found;
}

// =============================================================================
// Resource reservation
// =============================================================================

// Reserve resources immediately, including for tasks waiting for wakeup.
// This prevents subsequent arrivals from overcommitting the same machine.

static void ReserveTask(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    double demand = GetTaskDemand(task_id);

    unsigned memory = GetTaskMemory(task_id);

    g_machine_demand[machine_id] += demand;
    g_machine_memory[machine_id] += memory;

    if (!g_machine_vm_reserved[machine_id]) {
        g_machine_memory[machine_id] += VM_MEMORY_OVERHEAD;
        g_machine_vm_reserved[machine_id] = true;
    }

    g_task_demand[task_id] = demand;
    g_task_memory[task_id] = memory;
    g_task_machine[task_id] = machine_id;
}

static void ReleaseTask(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    auto demand_it = g_task_demand.find(task_id);

    if (demand_it != g_task_demand.end()) {
        g_machine_demand[machine_id] -= demand_it->second;

        if (g_machine_demand[machine_id] < 0.0)
            g_machine_demand[machine_id] = 0.0;

        g_task_demand.erase(demand_it);
    }

    auto memory_it = g_task_memory.find(task_id);

    if (memory_it != g_task_memory.end()) {
        unsigned memory = memory_it->second;

        if (g_machine_memory[machine_id] >= memory)
            g_machine_memory[machine_id] -= memory;
        else
            g_machine_memory[machine_id] = 0;

        g_task_memory.erase(memory_it);
    }

    g_task_machine.erase(task_id);
}

// Release the shared VM overhead if no VM exists and no tasks are pending.
static void ReleaseVMOverheadIfUnused(MachineId_t machine_id)
{
    if (g_machine_has_vm[machine_id])
        return;

    auto pending = g_pending_tasks.find(machine_id);
    if (pending != g_pending_tasks.end() && !pending->second.empty())
        return;

    if (g_machine_vm_reserved[machine_id]) {
        if (g_machine_memory[machine_id] >= VM_MEMORY_OVERHEAD)
            g_machine_memory[machine_id] -= VM_MEMORY_OVERHEAD;
        else
            g_machine_memory[machine_id] = 0;

        g_machine_vm_reserved[machine_id] = false;
    }
}

// =============================================================================
// VM creation
// =============================================================================

// Resources have already been reserved before this function is called.

static void CreateVMForTask(
    MachineId_t machine_id,
    TaskId_t task_id)
{
    const MachineInfo_t& machine = g_machine_info[machine_id];

    VMId_t vm_id;

    if (g_machine_has_vm[machine_id]) {
        vm_id = g_machine_vm[machine_id];
    } else {
        vm_id = VM_Create(RequiredVMType(task_id), machine.cpu);
        VM_Attach(vm_id, machine_id);

        g_machine_vm[machine_id] = vm_id;
        g_machine_vm_type[machine_id] = RequiredVMType(task_id);
        g_machine_has_vm[machine_id] = true;
        ++g_machine_active_vms[machine_id];
    }

    VM_AddTask(
        vm_id,
        task_id,
        static_cast<Priority_t>(GetTaskPriority(task_id)));

    g_task_vm[task_id] = vm_id;
    ++g_machine_active_tasks[machine_id];
}

// =============================================================================
// Machine power management
// =============================================================================

static void RequestWakeup(MachineId_t machine_id)
{
    if (g_waking_machines.count(machine_id))
        return;

    if (g_powering_down_machines.count(machine_id))
        return;

    if (g_machine_state[machine_id] == S0)
        return;

    g_waking_machines.insert(machine_id);

    SimOutput(
        "Utility scheduler: waking machine "
        + to_string(machine_id),
        1);

    Machine_SetState(machine_id, S0);
}

static void PowerDownIfEmpty(MachineId_t machine_id)
{
    // Intentionally keep machines in S0 during the workload.
    //
    // This workload has frequent arrivals. Immediately transitioning an
    // empty machine to S5 can cause repeated S0 <-> S5 transitions, which
    // are expensive simulator events and can dominate execution time.
    //
    // A future version can add an idle timeout or periodic consolidation
    // policy instead of powering a machine down after every task.
    (void)machine_id;
}

// =============================================================================
// Scheduler methods
// =============================================================================

void Scheduler::Init()
{
    g_machine_info.clear();
    g_machine_state.clear();

    g_machine_memory.clear();
    g_machine_active_vms.clear();
    g_machine_vm.clear();
    g_machine_active_tasks.clear();
    g_machine_has_vm.clear();
    g_machine_vm_reserved.clear();
    g_machine_vm_type.clear();
    g_machine_demand.clear();

    g_task_vm.clear();
    g_task_machine.clear();
    g_task_demand.clear();
    g_task_memory.clear();

    g_pending_tasks.clear();
    g_task_pending_machine.clear();

    g_waking_machines.clear();
    g_powering_down_machines.clear();

    machines.clear();
    vms.clear();

    unsigned total_machines = Machine_GetTotal();

    for (MachineId_t i = 0; i < total_machines; ++i) {
        MachineInfo_t machine = Machine_GetInfo(i);

        machines.push_back(i);

        g_machine_info.push_back(machine);
        g_machine_state.push_back(machine.s_state);

        g_machine_memory.push_back(machine.memory_used);
        g_machine_active_vms.push_back(machine.active_vms);
        g_machine_vm.push_back(0);
        g_machine_active_tasks.push_back(0);
        g_machine_has_vm.push_back(false);
        g_machine_vm_reserved.push_back(false);
        g_machine_vm_type.push_back(LINUX);

        g_machine_demand.push_back(0.0);
    }

    SimOutput(
        "Initialized optimized SLA-aware utility scheduler",
        1);
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id)
{
    TaskInfo_t task = GetTaskInfo(task_id);

    if (task.completed)
        return;

    // Calculate demand once and cache it.
    g_task_demand[task_id] = GetTaskDemand(task_id);

    MachineId_t destination;

    if (!FindBestMachine(task_id, destination)) {
        g_task_demand.erase(task_id);

        SimOutput(
            "Utility scheduler: no feasible machine for task "
            + to_string(task_id),
            1);

        return;
    }

    // Reserve resources before creating the VM or requesting wakeup.
    ReserveTask(destination, task_id);

    // If the machine is already active and not transitioning,
    // create the VM immediately.
    if (g_machine_state[destination] == S0 &&
        !g_waking_machines.count(destination)) {

        CreateVMForTask(destination, task_id);
    } else {
        g_pending_tasks[destination].insert(task_id);
        g_task_pending_machine[task_id] = destination;

        SimOutput(
            "Utility scheduler: task "
            + to_string(task_id)
            + " waiting for machine "
            + to_string(destination),
            2);

        RequestWakeup(destination);
    }
}


void Scheduler::TaskComplete(Time_t now, TaskId_t task_id)
{
    // A task may complete before its machine finishes waking up.
    auto pending_it = g_task_pending_machine.find(task_id);
    if (pending_it != g_task_pending_machine.end()) {
        MachineId_t machine_id = pending_it->second;

        g_pending_tasks[machine_id].erase(task_id);
        g_task_pending_machine.erase(pending_it);

        ReleaseTask(machine_id, task_id);
        ReleaseVMOverheadIfUnused(machine_id);
        return;
    }

    // The simulator's completion callback occurs after the task has
    // disappeared from the VM's active-task list (as indicated by the
    // earlier VM::RemoveTask "non existent task" exception).
    //
    // Therefore, do not call VM_GetInfo() or VM_RemoveTask() here.
    // Both are unnecessary for scheduler bookkeeping, and VM_GetInfo()
    // copies the VM's active_tasks vector on every completion.

    auto vm_it = g_task_vm.find(task_id);
    if (vm_it == g_task_vm.end())
        return;

    auto machine_it = g_task_machine.find(task_id);
    if (machine_it == g_task_machine.end()) {
        g_task_vm.erase(vm_it);
        return;
    }

    MachineId_t machine_id = machine_it->second;
    VMId_t vm_id = vm_it->second;

    g_task_vm.erase(vm_it);
    ReleaseTask(machine_id, task_id);

    if (g_machine_active_tasks[machine_id] > 0)
        --g_machine_active_tasks[machine_id];

    // Each physical machine has at most one shared VM in this algorithm.
    // Shut it down after the final task completes.
    if (g_machine_active_tasks[machine_id] == 0 &&
        g_machine_has_vm[machine_id]) {

        VM_Shutdown(vm_id);
        g_machine_has_vm[machine_id] = false;

        if (g_machine_active_vms[machine_id] > 0)
            --g_machine_active_vms[machine_id];

        ReleaseVMOverheadIfUnused(machine_id);
    }
}

void Scheduler::PeriodicCheck(Time_t now)
{
    // No periodic migrations in this algorithm.
    // Placement decisions are made when tasks arrive.
}

void Scheduler::MigrationComplete(Time_t now, VMId_t vm_id)
{
    // No migrations are initiated by this algorithm.
}

void Scheduler::Shutdown(Time_t now)
{
    SimOutput(
        "Utility scheduler shutdown at time "
        + to_string(now),
        1);
}

// =============================================================================
// Public simulator callbacks
// =============================================================================

static Scheduler scheduler;

void InitScheduler()
{
    scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id)
{
    scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id)
{
    scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id)
{
    SimOutput(
        "Utility scheduler: memory warning on machine "
        + to_string(machine_id),
        1);
}

void MigrationDone(Time_t time, VMId_t vm_id)
{
    scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time)
{
    scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time)
{
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;

    cout << "Total Energy "
         << Machine_GetClusterEnergy()
         << "KW-Hour" << endl;

    cout << "Simulation run finished in "
         << double(time) / 1000000
         << " seconds" << endl;
    
    scheduler.Shutdown(time);

    SimOutput(
        "Simulation completed at time "
        + to_string(time),
        1);
}

void SLAWarning(Time_t time, TaskId_t task_id)
{
    // Give an at-risk task the highest scheduling priority.
    SetTaskPriority(task_id, HIGH_PRIORITY);

    SimOutput(
        "Utility scheduler: SLA warning for task "
        + to_string(task_id)
        + "; priority increased",
        1);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id)
{
    // Refresh only the machine that just completed a transition.
    MachineInfo_t actual =
        Machine_GetInfo(machine_id);

    g_machine_state[machine_id] = actual.s_state;

    g_waking_machines.erase(machine_id);
    g_powering_down_machines.erase(machine_id);

    if (actual.s_state != S0) {
        SimOutput(
            "Utility scheduler: machine "
            + to_string(machine_id)
            + " transitioned to state "
            + to_string(static_cast<unsigned>(actual.s_state)),
            2);

        return;
    }

    SimOutput(
        "Utility scheduler: machine "
        + to_string(machine_id)
        + " is now active",
        1);

    // Move pending tasks onto the machine.
    auto pending_it =
        g_pending_tasks.find(machine_id);

    if (pending_it != g_pending_tasks.end()) {
        set<TaskId_t> pending = pending_it->second;

        g_pending_tasks.erase(pending_it);

        for (TaskId_t task_id : pending) {
            g_task_pending_machine.erase(task_id);

            if (IsTaskCompleted(task_id)) {
                ReleaseTask(machine_id, task_id);
                continue;
            }

            // Resources were already reserved when the task arrived.
            CreateVMForTask(machine_id, task_id);
        }
    }

    ReleaseVMOverheadIfUnused(machine_id);
    PowerDownIfEmpty(machine_id);
}

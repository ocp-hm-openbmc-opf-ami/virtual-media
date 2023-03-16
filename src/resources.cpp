#include "resources.hpp"

#include "interfaces/mount_point_state_machine.hpp"

namespace resource
{

Process::~Process()
{
    if (spawned)
    {
        process->stop([& machine = *machine] {
            boost::asio::post(machine.getIoc(), [&machine]() {
                machine.emitSubprocessStoppedEvent();
            });
        });
    }
}

Gadget::Gadget(interfaces::MountPointStateMachine& machine,
               StateChange devState) :
    machine(&machine)
{
    status = UsbGadget::configure(
        std::string(machine.getName()), machine.getConfig().nbdDevice, devState,
        machine.getTarget() ? machine.getTarget()->rw : false);
}

Gadget::~Gadget()
{
    UsbGadget::configure(std::string(machine->getName()),
                         machine->getConfig().nbdDevice, StateChange::removed);
}

} // namespace resource

#include "resources.hpp"

#include "interfaces/mount_point_state_machine.hpp"

namespace resource
{

Process::~Process()
{
    if (spawned)
    {
        process->stop([&machine = *machine] {
            boost::asio::post(machine.getIoc(), [&machine]() {
                machine.emitSubprocessStoppedEvent();
            });
        });
    }
}

Gadget::Gadget(interfaces::MountPointStateMachine& machine,
               StateChange devState) : machine(&machine)
{
    try
    {
        status = UsbGadget::configure(
            std::string(machine.getName()), machine.getConfig().nbdDevice,
            devState, machine.getTarget() ? machine.getTarget()->rw : false,
            std::string(machine.getAdditionalInfo()));

        if (status == -2)
        {
            LogMsg(Logger::Error,
                   "Failed to configure USB gadget for: ", machine.getName(),
                   " - Image size is below minimum supported limit (600KB)");
        }
        else if (status == -1)
        {
            LogMsg(
                Logger::Error,
                "Failed to configure USB gadget for: ", machine.getName(),
                " - Check system logs for details (image size, USB hub, or filesystem error)");
        }
    }
    catch (const std::exception& e)
    {
        LogMsg(Logger::Error, "Failed to configure USB gadget for ",
               machine.getName(), ": ", e.what());
        status = -1;
    }
}

Gadget::~Gadget()
{
    UsbGadget::configure(std::string(machine->getName()),
                         machine->getConfig().nbdDevice, StateChange::removed);
}

} // namespace resource

#include "active_state.hpp"

#include "deactivating_state.hpp"

ActiveState::ActiveState(interfaces::MountPointStateMachine& machine,
                         std::unique_ptr<resource::Process> process,
                         std::unique_ptr<resource::Gadget> gadget) :
    BasicStateT(machine),
    process(std::move(process)), gadget(std::move(gadget)){};

ActiveState::ActiveState(interfaces::MountPointStateMachine& machine) :
    BasicStateT(machine)
{
    lastStats = "";
    lastAccess = std::chrono::steady_clock::now();
};

std::unique_ptr<BasicState> ActiveState::onEnter()
{
    if (Configuration::inactivityTimeout == std::chrono::seconds(0))
    {
        LogMsg(Logger::Info, "InactivityTimeout disabled");
        return nullptr;
    }

    handler = [this](const boost::system::error_code& ec) {
        if (ec)
        {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        std::optional<std::string> stats;

        if (machine.getConfig().mode == Configuration::Mode::internal)
        {
            if (machine.getDest() ==
                interfaces::MountPointStateMachine::Dest::host)
            {
                stats = UsbGadget::getStats(machine.getConfig().ramDisk);
            }
            else if (machine.getDest() ==
                     interfaces::MountPointStateMachine::Dest::bmc)
            {
                //@TODO: check the disk status
            }
            else
            {
                LogMsg(Logger::Info, machine.getName(),
                       " invalid unmount target, .");
            }
        }
        else
        {
            stats = UsbGadget::getStats(std::string(machine.getName()));
        }

        if (stats && (*stats != lastStats))
        {
            lastStats = std::move(*stats);
            lastAccess = now;
        }

        auto timeSinceLastAccess =
                std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                 lastAccess);
        if (timeSinceLastAccess >= Configuration::inactivityTimeout)
        {
            LogMsg(Logger::Info, machine.getName(),
                   " Inactivity timer expired (",
                       Configuration::inactivityTimeout.count(),
                       "s) - Unmounting");
            // unmount media & stop retriggering timer
            boost::asio::spawn(
                machine.getIoc(),
                [&machine = machine](boost::asio::yield_context yield) {
                    if (machine.getConfig().mode ==
                        Configuration::Mode::internal)
                    {
                        if (machine.getDest() ==
                            interfaces::MountPointStateMachine::Dest::host)
                        {
                            machine.emitUnmountEvent(
                                interfaces::MountPointStateMachine::Dest::host);
                        }
                        else
                        {
                            LogMsg(Logger::Info, machine.getName(),
                                   " invalid unmount target, .");
                        }
                    }
                    else
                        machine.emitUnmountEvent();
                });
            return;
        }
        else
        {
            machine.getConfig().remainingInactivityTimeout =
                Configuration::inactivityTimeout - timeSinceLastAccess;
        }

        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(handler);
    };
    timer.expires_from_now(std::chrono::seconds(1));
    timer.async_wait(handler);

    return nullptr;
}


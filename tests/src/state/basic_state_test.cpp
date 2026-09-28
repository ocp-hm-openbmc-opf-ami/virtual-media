// SPDX-License-Identifier: Apache-2.0
/**
 * @file basic_state_test.cpp
 * @brief Unit tests for BasicState/BasicStateT<T> CRTP base class
 *
 * This test suite validates the foundational CRTP (Curiously Recurring Template
 * Pattern) base class used by all state machine states. BasicStateT provides
 * the framework for type-safe state pattern implementation with compile-time
 * polymorphism.
 *
 * Testing Strategy:
 * - Concrete test states (DummyState, OtherState) for CRTP validation
 * - Mock MountPointStateMachine for interface testing
 * - Static assertions for type traits verification
 * - Runtime behavior validation of CRTP methods
 *
 * Functional Coverage:
 * 1. **CRTP Pattern Implementation**:
 *    - getStateName() returns correct derived type name
 *    - get_if<T>() template method for safe downcasting
 *    - Machine reference storage and access
 *
 * 2. **Type Safety Constraints**:
 *    - Non-copyable (deleted copy constructor/assignment)
 *    - Non-movable (deleted move constructor/assignment)
 *    - Virtual destructor for polymorphic deletion
 *
 * 3. **Event Handling Framework**:
 *    - handleEvent() template method dispatch
 *    - State transition return semantics (nullptr = stay, new state =
 * transition)
 *    - Event processing without transitions
 *
 * 4. **State Identity Operations**:
 *    - Runtime type identification via get_if<T>()
 *    - Null return for type mismatches
 *    - Valid pointer for correct type
 *
 * Test Coverage Areas:
 * - CRTP stateName() propagation
 * - State machine reference access
 * - Type-safe downcasting (get_if)
 * - Event handling dispatch
 * - State transition semantics
 * - Type trait validation
 *
 * Edge Cases Tested:
 * - get_if with wrong type (returns nullptr)
 * - get_if with correct type (returns valid pointer)
 * - Event handling with/without transitions
 * - Multiple derived state types
 *
 * Dependencies:
 * - MockMountPointStateMachine: Interface compliance
 * - std::type_traits: Compile-time type validation
 *
 * @note BasicState uses CRTP for zero-cost abstraction
 * @note All state classes must derive from BasicStateT<DerivedState>
 */
// Unit tests for BasicState / BasicStateT<T> from basic_state.hpp

#include "state/basic_state.hpp" // includes events.hpp + mount_point_state_machine.hpp

#include <filesystem>
#include <memory>
#include <string_view>
#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// ---- GMock for interfaces::MountPointStateMachine --------------------------
namespace interfaces
{

class MockMountPointStateMachine : public MountPointStateMachine
{
  public:
    MOCK_METHOD(void, notify, (const std::error_code& ec), (override));
    MOCK_METHOD(void, notificationStart, (), (override));
    MOCK_METHOD(void, notificationInitialize,
                (std::shared_ptr<sdbusplus::asio::connection> con,
                 const std::string& svc, const std::string& iface,
                 const std::string& name),
                (override));

    MOCK_METHOD(std::string_view, getName, (), (const, override));

    MOCK_METHOD(Configuration::MountPoint&, getConfig, (), (override));
    MOCK_METHOD(std::optional<Target>&, getTarget, (), (override));

    MOCK_METHOD(BasicState&, getState, (), (override));
    MOCK_METHOD(int&, getExitCode, (), (override));
    MOCK_METHOD(boost::asio::io_context&, getIoc, (), (override));

    MOCK_METHOD(std::string_view, getAdditionalInfo, (), (const, override));
    MOCK_METHOD(void, setAdditionalInfo, (const std::string&), (override));

    MOCK_METHOD(void, emitRegisterDBusEvent,
                (std::shared_ptr<sdbusplus::asio::connection> bus,
                 std::shared_ptr<sdbusplus::asio::object_server> objServer),
                (override));

    MOCK_METHOD(void, emitMountEvent, (std::optional<Target>), (override));
    MOCK_METHOD(void, emitUnmountEvent, (), (override));
    MOCK_METHOD(void, emitSubprocessStoppedEvent, (), (override));
    MOCK_METHOD(void, emitUdevStateChangeEvent,
                (const NBDDevice& dev, StateChange devState), (override));
};

} // namespace interfaces

// ---- Test helper concrete states -------------------------------------------
struct DummyState : public BasicStateT<DummyState>
{
    using Base = BasicStateT<DummyState>;
    explicit DummyState(interfaces::MountPointStateMachine& m) : Base(m) {}
    static constexpr std::string_view stateName()
    {
        return "DummyState";
    }

    // Generic handler: marks that we saw an event, no transition.
    template <class E>
    std::unique_ptr<BasicState> handleEvent(E event)
    {
        sawEvent = true;
        if constexpr (requires { event.eventName; })
        {
            lastEventName = event.eventName;
        }
        else
        {
            lastEventName = std::visit(
                [](const auto& innerEvent) { return innerEvent.eventName; },
                event);
        }
        return nullptr;
    }

    bool sawEvent{false};
    const char* lastEventName{nullptr};
};

struct OtherState : public BasicStateT<OtherState>
{
    using Base = BasicStateT<OtherState>;
    explicit OtherState(interfaces::MountPointStateMachine& m) : Base(m) {}
    static constexpr std::string_view stateName()
    {
        return "OtherState";
    }

    // Always transition to DummyState for testing.
    template <class E>
    std::unique_ptr<BasicState> handleEvent(E)
    {
        return std::make_unique<DummyState>(machine);
    }
};

// ---- Type-trait checks ------------------------------------------------------
static_assert(!std::is_copy_constructible_v<BasicState>,
              "BasicState must be non-copyable");
static_assert(!std::is_copy_assignable_v<BasicState>,
              "BasicState must be non-copy-assignable");
static_assert(!std::is_move_constructible_v<BasicState>,
              "BasicState must be non-movable");
static_assert(!std::is_move_assignable_v<BasicState>,
              "BasicState must be non-move-assignable");

// ---- Tests ------------------------------------------------------------------

TEST(BasicStateCore, ConstructionAndStateName)
{
    interfaces::MockMountPointStateMachine machine;

    DummyState s{machine};

    // getStateName() forwards to T::stateName() through CRTP.
    EXPECT_EQ(s.getStateName(), DummyState::stateName());
    EXPECT_NE(s.getStateName(), OtherState::stateName());

    BasicState& baseRef = s; // upcast sanity
    (void)baseRef;
}

TEST(BasicStateCore, BaseClassStoresMachineReference)
{
    interfaces::MockMountPointStateMachine machine;

    DummyState s{machine};
    BasicState* base = &s;

    EXPECT_EQ(&s.machine, &machine);
    EXPECT_EQ(&base->machine, &machine);
}

TEST(BasicStateCore, GetIfReturnsMatchingTypeOrNull)
{
    interfaces::MockMountPointStateMachine machine;

    DummyState s{machine};
    OtherState o{machine};

    EXPECT_EQ(s.get_if<DummyState>(), &s);
    EXPECT_EQ(s.get_if<OtherState>(), nullptr);

    EXPECT_EQ(o.get_if<OtherState>(), &o);
    EXPECT_EQ(o.get_if<DummyState>(), nullptr);
}

TEST(BasicStateCore, GetIfWorksThroughBasePointer)
{
    interfaces::MockMountPointStateMachine machine;

    DummyState s{machine};
    BasicState* base = &s;

    EXPECT_EQ(base->get_if<DummyState>(), &s);
    EXPECT_EQ(base->get_if<OtherState>(), nullptr);
}

TEST(BasicStateCore, OnEnterDefaultReturnsNullptr)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    // Default onEnter() in BasicStateT<T> returns nullptr.
    EXPECT_EQ(s.onEnter(), nullptr);
}

TEST(BasicStateCore, HandleEventInvokesTemplatedHandlerAndNoTransition)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    // Construct a real Event using UnmountEvent (default-constructible).
    Event ev{std::in_place_type<UnmountEvent>};

    // This calls BasicStateT<T>::handleEvent(Event) which std::visit's and
    // forwards to T::handleEvent(e).
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);  // our handler ran
    EXPECT_EQ(next, nullptr); // no transition requested
}

TEST(BasicStateCore, HandleEventRecordsUnmountEventName)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    Event ev{std::in_place_type<UnmountEvent>};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_STREQ(s.lastEventName, "UnmountEvent");
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, BaseReferenceDispatchesMountEventVariant)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};
    BasicState& base = s;

    interfaces::MountPointStateMachine::Target target{};
    target.imgUrl = "https://example.com/mount.iso";
    target.rw = false;

    Event ev{std::in_place_type<MountEvent>,
             std::optional<interfaces::MountPointStateMachine::Target>{
                 std::move(target)}};
    auto next = base.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventDispatchesRegisterEventVariant)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    Event ev{std::in_place_type<RegisterDbusEvent>, nullptr, nullptr};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventRecordsRegisterDbusEventName)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    Event ev{std::in_place_type<RegisterDbusEvent>, nullptr, nullptr};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_STREQ(s.lastEventName, "RegisterDbusEvent");
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventRecordsMountEventName)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    interfaces::MountPointStateMachine::Target target{};
    target.imgUrl = "https://example.com/mount.iso";
    Event ev{std::in_place_type<MountEvent>,
             std::optional<interfaces::MountPointStateMachine::Target>{
                 std::move(target)}};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_STREQ(s.lastEventName, "MountEvent");
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, BaseReferenceDispatchesSubprocessStoppedVariant)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};
    BasicState& base = s;

    Event ev{std::in_place_type<SubprocessStoppedEvent>};
    auto next = base.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventRecordsSubprocessStoppedEventName)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    Event ev{std::in_place_type<SubprocessStoppedEvent>};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_STREQ(s.lastEventName, "SubprocessStoppedEvent");
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, BaseReferenceDispatchesUdevStateChangeVariant)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};
    BasicState& base = s;

    Event ev{std::in_place_type<UdevStateChangeEvent>, StateChange::inserted};
    auto next = base.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventRecordsUdevStateChangeEventName)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};

    Event ev{std::in_place_type<UdevStateChangeEvent>, StateChange::inserted};
    auto next = s.handleEvent(std::move(ev));

    EXPECT_TRUE(s.sawEvent);
    EXPECT_STREQ(s.lastEventName, "UdevStateChangeEvent");
    EXPECT_EQ(next, nullptr);
}

TEST(BasicStateCore, HandleEventCanReturnTransitionToAnotherState)
{
    interfaces::MockMountPointStateMachine machine;
    OtherState s{machine};

    Event ev{std::in_place_type<UnmountEvent>};
    auto next = s.handleEvent(std::move(ev));

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), DummyState::stateName());
    auto* asDummy = next->get_if<DummyState>();
    ASSERT_NE(asDummy, nullptr);
}

TEST(BasicStateCore, BaseReferenceCanReturnTransitionToAnotherState)
{
    interfaces::MockMountPointStateMachine machine;
    OtherState s{machine};
    BasicState& base = s;

    Event ev{std::in_place_type<RegisterDbusEvent>, nullptr, nullptr};
    auto next = base.handleEvent(std::move(ev));

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->getStateName(), DummyState::stateName());
    EXPECT_NE(next->get_if<DummyState>(), nullptr);
}

TEST(BasicStateCore, TransitionKeepsOriginalMachineReference)
{
    interfaces::MockMountPointStateMachine machine;
    OtherState s{machine};

    Event ev{std::in_place_type<UnmountEvent>};
    auto next = s.handleEvent(std::move(ev));

    ASSERT_NE(next, nullptr);
    EXPECT_EQ(&next->machine, &machine);
}

TEST(BasicStateCore, BaseReferenceOnEnterUsesDefaultImplementation)
{
    interfaces::MockMountPointStateMachine machine;
    DummyState s{machine};
    BasicState& base = s;

    EXPECT_EQ(base.onEnter(), nullptr);
}

TEST(BasicStateCore, DestroyingPolymorphicDummyStateIsSafe)
{
    interfaces::MockMountPointStateMachine machine;
    std::unique_ptr<BasicState> state = std::make_unique<DummyState>(machine);

    EXPECT_EQ(state->getStateName(), DummyState::stateName());
}

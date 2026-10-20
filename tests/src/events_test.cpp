// SPDX-License-Identifier: Apache-2.0
/**
 * @file events_test.cpp
 * @brief Unit tests for event types used in virtual media state machine
 *
 * This test suite validates all event structures used for state machine
 * communication. Events trigger state transitions and carry payloads for
 * state logic decisions.
 *
 * Testing Strategy:
 * - Type traits verification (move-only semantics)
 * - Event name string validation
 * - Payload preservation and ownership transfer
 * - std::variant integration testing
 * - Move semantics validation
 *
 * Event Types Covered:
 * 1. **RegisterDbusEvent**: D-Bus service registration trigger
 *    - Payload: bus connection + object server shared_ptrs
 *    - Triggers: InitialState → ReadyState transition
 *
 * 2. **MountEvent**: Mount operation request
 *    - Payload: optional<Target> (can be empty for proxy mode)
 *    - Move-only (Target may be non-copyable)
 *    - Triggers: ReadyState → ActivatingState transition
 *
 * 3. **UnmountEvent**: Unmount operation request
 *    - Payload: none
 *    - Triggers: ActiveState → DeactivatingState transition
 *
 * 4. **SubprocessStoppedEvent**: NBD process termination
 *    - Payload: none
 *    - Triggers: ActivatingState/ActiveState → DeactivatingState
 *
 * 5. **UdevStateChangeEvent**: USB gadget/NBD device state change
 *    - Payload: StateChange (inserted/removed)
 *    - Triggers: ActivatingState → ActiveState (on inserted)
 *    - Triggers: DeactivatingState cleanup completion
 *
 * Functional Coverage:
 * 1. **Type Traits Validation**:
 *    - MountEvent is NOT copy-constructible
 *    - MountEvent is NOT copy-assignable
 *    - MountEvent IS move-constructible
 *    - MountEvent IS move-assignable
 *
 * 2. **Event Name Verification**:
 *    - Each event has correct eventName string member
 *    - Names match class names for debugging
 *
 * 3. **Move Semantics**:
 *    - MountEvent move constructor clears source
 *    - MountEvent move assignment clears source
 *    - Target ownership properly transferred
 *    - Optional cleared to nullopt after move
 *
 * 4. **Variant Integration**:
 *    - Event type holds all event types
 *    - std::holds_alternative works correctly
 *    - std::get retrieves correct event
 *    - Variant can be constructed from any event type
 *
 * Test Coverage Areas:
 * - All event type constructors
 * - Event name member correctness
 * - Move operation behavior
 * - Variant type compatibility
 * - Payload preservation
 *
 * Edge Cases Tested:
 * - MountEvent with empty optional
 * - MountEvent with populated target
 * - Move-then-access behavior
 * - Variant type discrimination
 *
 * Implementation Notes:
 * - MountEvent uses move-only semantics to enforce ownership transfer
 * - Other events are lightweight (no payloads or simple payloads)
 * - Event type is std::variant for type-safe dispatch
 *
 * Dependencies:
 * - std::variant: Type-safe event union
 * - std::optional: Optional payloads
 * - sdbusplus: D-Bus types
 *
 * @note Events are the primary communication mechanism for state machine
 * @note Move semantics prevent accidental payload copying
 */
// tests/src/events_test.cpp
#include "events.hpp" // adjust include path if needed

#include <memory>
#include <optional>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

// Short alias for readability
using Target = interfaces::MountPointStateMachine::Target;

// ---------- Type traits for MountEvent ----------
TEST(MountEventTraitsTest, CopyIsDeletedMoveIsAllowed)
{
    static_assert(!std::is_copy_constructible_v<MountEvent>,
                  "MountEvent must NOT be copy constructible");
    static_assert(!std::is_copy_assignable_v<MountEvent>,
                  "MountEvent must NOT be copy assignable");
    static_assert(std::is_move_constructible_v<MountEvent>,
                  "MountEvent must be move constructible");
    static_assert(std::is_move_assignable_v<MountEvent>,
                  "MountEvent must be move assignable");
}

// ---------- EventName checks ----------
TEST(EventNameTest, RegisterDbusEventHasCorrectName)
{
    // These can be null; we are testing only storage and name.
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> server;
    RegisterDbusEvent e{bus, server};
    EXPECT_STREQ("RegisterDbusEvent", e.eventName);
    EXPECT_EQ(bus, e.bus);
    EXPECT_EQ(server, e.objServer);
}

TEST(EventNameTest, MountEventHasCorrectName)
{
    // Pass an rvalue optional to avoid copying (Target may be move-only).
    MountEvent e{std::optional<Target>{}};
    EXPECT_STREQ("MountEvent", e.eventName);
    EXPECT_FALSE(e.target.has_value());
}

// ---------- Variant integration ----------
TEST(VariantEventTest, HoldsCorrectAlternativeForEachType)
{
    // RegisterDbusEvent in variant
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> server;
    Event ev1 = RegisterDbusEvent{bus, server};
    ASSERT_TRUE(std::holds_alternative<RegisterDbusEvent>(ev1));
    const auto& r1 = std::get<RegisterDbusEvent>(ev1);
    EXPECT_STREQ("RegisterDbusEvent", r1.eventName);
    EXPECT_EQ(bus, r1.bus);
    EXPECT_EQ(server, r1.objServer);

    // MountEvent (non-empty optional) in variant
    Event ev2 = MountEvent{std::make_optional<Target>()};
    ASSERT_TRUE(std::holds_alternative<MountEvent>(ev2));
    const auto& m2 = std::get<MountEvent>(ev2);
    EXPECT_STREQ("MountEvent", m2.eventName);
    EXPECT_TRUE(m2.target.has_value());

    // UnmountEvent
    Event ev3 = UnmountEvent{};
    ASSERT_TRUE(std::holds_alternative<UnmountEvent>(ev3));
    EXPECT_STREQ("UnmountEvent", std::get<UnmountEvent>(ev3).eventName);

    // SubprocessStoppedEvent
    Event ev4 = SubprocessStoppedEvent{};
    ASSERT_TRUE(std::holds_alternative<SubprocessStoppedEvent>(ev4));
    EXPECT_STREQ("SubprocessStoppedEvent",
                 std::get<SubprocessStoppedEvent>(ev4).eventName);

    // UdevStateChangeEvent
    StateChange sc{};
    Event ev5 = UdevStateChangeEvent{sc};
    ASSERT_TRUE(std::holds_alternative<UdevStateChangeEvent>(ev5));
    EXPECT_STREQ("UdevStateChangeEvent",
                 std::get<UdevStateChangeEvent>(ev5).eventName);
}

// ---------------------------------------------
// MountEvent: non-empty optional & move semantics
// ---------------------------------------------

TEST(MountEventNonEmptyOptionalTest,
     MoveConstructorTransfersValueAndClearsSource)
{
    MountEvent src{std::make_optional<Target>()};
    ASSERT_TRUE(src.target.has_value());
    EXPECT_STREQ("MountEvent", src.eventName);

    MountEvent dst{std::move(src)};
    EXPECT_TRUE(dst.target.has_value());
    EXPECT_STREQ("MountEvent", dst.eventName);
    EXPECT_FALSE(src.target.has_value()); // source cleared to nullopt
}

TEST(MountEventNonEmptyOptionalTest, MoveAssignFromNonEmptyToEmpty)
{
    MountEvent empty{std::optional<Target>{}};
    MountEvent full{std::make_optional<Target>()};
    ASSERT_FALSE(empty.target.has_value());
    ASSERT_TRUE(full.target.has_value());

    empty = std::move(full);
    EXPECT_TRUE(empty.target.has_value());
    EXPECT_FALSE(full.target.has_value()); // source cleared to nullopt
    EXPECT_STREQ("MountEvent", empty.eventName);
}

TEST(MountEventNonEmptyOptionalTest,
     MoveAssignFromNonEmptyToNonEmptySwapsAndClearsSource)
{
    MountEvent a{std::make_optional<Target>()};
    MountEvent b{std::make_optional<Target>()};
    ASSERT_TRUE(a.target.has_value());
    ASSERT_TRUE(b.target.has_value());

    a = std::move(b);
    EXPECT_TRUE(a.target.has_value());
    EXPECT_FALSE(b.target.has_value()); // source cleared to nullopt
    EXPECT_STREQ("MountEvent", a.eventName);
}

// ---------------------------------------------
// UdevStateChangeEvent: devState content
// ---------------------------------------------

TEST(UdevStateChangeEventContentTest,
     MemberTypeIsStateChangeAndConstructionWorks)
{
    // Compile-time: member is stored by value as StateChange
    static_assert(
        std::is_same<decltype(std::declval<UdevStateChangeEvent>().devState),
                     StateChange>(),
        "UdevStateChangeEvent::devState must be of type StateChange");

    // Lvalue construction — verify name and member equality
    StateChange sc_lvalue{};
    UdevStateChangeEvent e{sc_lvalue};
    EXPECT_STREQ("UdevStateChangeEvent", e.eventName);
    EXPECT_EQ(e.devState, sc_lvalue);
}

TEST(UdevStateChangeEventContentTest, StoresAllStateChangeValues)
{
    UdevStateChangeEvent e_inserted{StateChange::inserted};
    EXPECT_EQ(StateChange::inserted, e_inserted.devState);

    UdevStateChangeEvent e_removed{StateChange::removed};
    EXPECT_EQ(StateChange::removed, e_removed.devState);

    UdevStateChangeEvent e_not_monitored{StateChange::notMonitored};
    EXPECT_EQ(StateChange::notMonitored, e_not_monitored.devState);
}

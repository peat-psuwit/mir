/*
 * Copyright © 2015-2016 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "mir/geometry/rectangle.h"
#include "mir/scene/session.h"
#include "mir_toolkit/events/surface_placement.h"
#include "mir/events/event_builders.h"
#include "mir/scene/surface.h"

#include "mir_toolkit/mir_client_library.h"

#include "mir_test_framework/headless_test.h"
#include "mir/test/doubles/mock_window_manager.h"

#include "mir/test/fake_shared.h"
#include "mir/test/signal.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace mf = mir::frontend;
namespace ms = mir::scene;
namespace msh = mir::shell;
namespace geom = mir::geometry;

namespace mt = mir::test;
namespace mtd = mir::test::doubles;
namespace mtf = mir_test_framework;
using namespace testing;
using namespace std::literals::chrono_literals;

namespace
{
MATCHER_P(WeakPtrEq, p, "")
{
    return !arg.owner_before(p) && !p.owner_before(arg);
}

std::vector<geom::Rectangle> const display_geometry
{
    {{  0, 0}, { 640,  480}},
    {{480, 0}, {1920, 1080}}
};


using NiceMockWindowManager = NiceMock<mtd::MockWindowManager>;

struct Client
{
    explicit Client(char const* connect_string) :
        connection{mir_connect_sync(connect_string, __PRETTY_FUNCTION__)}
    {
    }

    Client(Client&& source) :
        connection{nullptr}
    {
        std::swap(connection, source.connection);
    }

    auto surface_create() const -> MirSurface*
    {
        auto spec = mir_specify_window(connection, 800, 600, mir_pixel_format_bgr_888);
        auto surface = mir_surface_create_sync(spec);
        mir_surface_spec_release(spec);

        return surface;
    }

    void disconnect()
    {
        if (connection)
            mir_connection_release(connection);

        connection = nullptr;
    }

    ~Client() noexcept
    {
        disconnect();
    }

    MirConnection* connection{nullptr};
};

struct CustomWindowManagement : mtf::HeadlessTest
{
    void SetUp() override
    {
        add_to_environment("MIR_SERVER_NO_FILE", "");

        initial_display_layout(display_geometry);

        server.override_the_window_manager_builder([this]
            (msh::FocusController*) { return mt::fake_shared(window_manager); });
    }

    void TearDown() override
    {
        stop_server();
    }

    Client connect_client()
    {
        return Client(new_connection().c_str());
    }

    NiceMockWindowManager window_manager;
};
}

TEST_F(CustomWindowManagement, display_layout_is_notified_on_startup)
{
    for(auto const& rect : display_geometry)
        EXPECT_CALL(window_manager, add_display(rect));

    start_server();
}

TEST_F(CustomWindowManagement, display_layout_is_notified_on_shutdown)
{
    start_server();

    for(auto const& rect : display_geometry)
        EXPECT_CALL(window_manager, remove_display(rect));
}

TEST_F(CustomWindowManagement, client_connect_adds_session)
{
    start_server();

    EXPECT_CALL(window_manager, add_session(_));
    connect_client();
}

TEST_F(CustomWindowManagement, client_disconnect_removes_session)
{
    start_server();
    auto client = connect_client();

    EXPECT_CALL(window_manager, remove_session(_));

    client.disconnect();
}

TEST_F(CustomWindowManagement, surface_create_adds_surface)
{
    start_server();

    auto const client = connect_client();

    EXPECT_CALL(window_manager, add_surface(_,_,_));

    auto const surface = client.surface_create();

    mir_surface_release_sync(surface);
}

TEST_F(CustomWindowManagement, surface_rename_modifies_surface)
{
    auto const new_title = __PRETTY_FUNCTION__;

    start_server();
    auto const client = connect_client();
    auto const surface = client.surface_create();

    EXPECT_CALL(window_manager, modify_surface(_,_,_));

    auto const spec = mir_create_spec(client.connection);
    mir_surface_spec_set_name(spec, new_title);
    mir_surface_apply_spec(surface, spec);
    mir_surface_spec_release(spec);

    mir_surface_release_sync(surface);
}

TEST_F(CustomWindowManagement, surface_release_removes_surface)
{
    start_server();

    auto const client = connect_client();
    auto const surface = client.surface_create();

    EXPECT_CALL(window_manager, remove_surface(_,_));

    mir_surface_release_sync(surface);

    Mock::VerifyAndClearExpectations(&window_manager);
}

TEST_F(CustomWindowManagement, client_disconnect_removes_unreleased_surfaces)
{
    start_server();
    auto client = connect_client();
    client.surface_create();

    EXPECT_CALL(window_manager, remove_surface(_,_));

    client.disconnect();
}

TEST_F(CustomWindowManagement, surface_is_associated_with_correct_client)
{
    start_server();

    const int no_of_clients = 17;
    std::weak_ptr<ms::Session> session[no_of_clients];
    std::vector<Client> client; client.reserve(no_of_clients);

    for (int i = 0; i != no_of_clients; ++i)
    {
        EXPECT_CALL(window_manager, add_session(_)).WillOnce(SaveArg<0>(&session[i]));

        client.emplace_back(connect_client());
    }

    Mock::VerifyAndClearExpectations(&window_manager);

    for (int i = 0; i != no_of_clients; ++i)
    {
        EXPECT_CALL(window_manager, add_surface(WeakPtrEq(session[i]),_,_));
        EXPECT_CALL(window_manager, remove_surface(WeakPtrEq(session[i]),_));

        auto const surface = client[i].surface_create();
        mir_surface_release_sync(surface);

        // verify expectations for each client in turn
        Mock::VerifyAndClearExpectations(&window_manager);
    }
}

TEST_F(CustomWindowManagement, state_change_requests_are_associated_with_correct_surface)
{
    start_server();
    auto const client = connect_client();

    const int no_of_surfaces = 17;
    std::weak_ptr<ms::Surface> server_surface[no_of_surfaces];
    MirSurface* client_surface[no_of_surfaces] = {};

    for (int i = 0; i != no_of_surfaces; ++i)
    {
        auto const grab_server_surface = [i, &server_surface](
            std::shared_ptr<ms::Session> const& session,
            ms::SurfaceCreationParameters const& params,
            std::function<mf::SurfaceId(std::shared_ptr<ms::Session> const& session, ms::SurfaceCreationParameters const& params)> const& build)
            {
                auto const result = build(session, params);
                server_surface[i] = session->surface(result);
                return result;
            };

        EXPECT_CALL(window_manager, add_surface(_,_,_))
            .WillOnce(Invoke(grab_server_surface));

        client_surface[i] = client.surface_create();
    }

    for (int i = 0; i != no_of_surfaces; ++i)
    {
        // verify expectations for each surface in turn
        Mock::VerifyAndClearExpectations(&window_manager);

        mt::Signal received;

        EXPECT_CALL(window_manager, set_surface_attribute(_, WeakPtrEq(server_surface[i]), mir_surface_attrib_state,_))
            .WillOnce(WithArg<3>(Invoke([&](int value) { received.raise(); return value; })));

        mir_surface_set_state(client_surface[i], mir_surface_state_maximized);

        received.wait_for(400ms);
    }

    for (auto const surface : client_surface)
        mir_surface_release_sync(surface);
}

TEST_F(CustomWindowManagement, create_low_chrome_surface_from_spec)
{
    start_server();

    auto connection = mir_connect_sync(new_connection().c_str(), __PRETTY_FUNCTION__);

    int const width{800}, height{600};
    MirPixelFormat const format{mir_pixel_format_bgr_888};
    auto surface_spec = mir_specify_window(connection, width, height, format);

    mir_surface_spec_set_shell_chrome(surface_spec, mir_shell_chrome_low);

    auto const check_add_surface = [](
        std::shared_ptr<ms::Session> const& session,
        ms::SurfaceCreationParameters const& params,
        std::function<mf::SurfaceId(std::shared_ptr<ms::Session> const& session, ms::SurfaceCreationParameters const& params)> const& build)
            {
                EXPECT_TRUE(params.shell_chrome.is_set());
                return build(session, params);
            };

    EXPECT_CALL(window_manager, add_surface(_,_,_)).WillOnce(Invoke(check_add_surface));

    auto surface = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    mir_surface_release_sync(surface);
    mir_connection_release(connection);
}

TEST_F(CustomWindowManagement, apply_low_chrome_to_surface)
{
    start_server();

    auto connection = mir_connect_sync(new_connection().c_str(), __PRETTY_FUNCTION__);

    int const width{800}, height{600};
    MirPixelFormat const format{mir_pixel_format_bgr_888};
    auto surface_spec = mir_specify_window(connection, width, height, format);

    auto surface = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    surface_spec = mir_create_spec(connection);

    mt::Signal received;

    mir_surface_spec_set_shell_chrome(surface_spec, mir_shell_chrome_low);

    auto const check_apply_surface = [&received](
        std::shared_ptr<ms::Session> const&,
        std::shared_ptr<ms::Surface> const&,
        msh::SurfaceSpecification const& spec)
        {
            EXPECT_TRUE(spec.shell_chrome.is_set());
            received.raise();
        };

    EXPECT_CALL(window_manager, modify_surface(_,_,_)).WillOnce(Invoke(check_apply_surface));

    mir_surface_apply_spec(surface, surface_spec);
    mir_surface_spec_release(surface_spec);

    EXPECT_TRUE(received.wait_for(400ms));

    mir_surface_release_sync(surface);
    mir_connection_release(connection);
}

TEST_F(CustomWindowManagement, when_the_client_places_a_new_surface_the_request_reaches_the_window_manager)
{
    int const width{800};
    int const height{600};
    MirPixelFormat const format{mir_pixel_format_bgr_888};
    MirRectangle dummy_rect{13, 17, 19, 23};
    MirRectangle const aux_rect{20, 20, 50, 50};
    auto const rect_gravity = mir_placement_gravity_northeast;
    auto const surface_gravity = mir_placement_gravity_northwest;
    auto const placement_hints = mir_placement_hints_flip_x;
    auto const offset_dx = 2;
    auto const offset_dy = 3;

    start_server();
    auto connection = mir_connect_sync(new_connection().c_str(), __PRETTY_FUNCTION__);
    auto surface_spec = mir_specify_window(connection, width, height, format);
    auto parent = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    surface_spec = mir_specify_tip(
        connection, width, height, format, parent, &dummy_rect, mir_edge_attachment_any);

    mir_surface_spec_set_placement(
        surface_spec, &aux_rect, rect_gravity, surface_gravity, placement_hints, offset_dx, offset_dy);

    mt::Signal received;

    auto const check_placement = [&](
        std::shared_ptr<ms::Session> const& session,
        ms::SurfaceCreationParameters const& params,
        std::function<mf::SurfaceId(std::shared_ptr<ms::Session> const& session, ms::SurfaceCreationParameters const& params)> const& build)
        {
            EXPECT_TRUE(params.aux_rect.is_set());
            if (params.aux_rect.is_set())
            {
                auto const actual_rect = params.aux_rect.value();
                EXPECT_THAT(actual_rect.top_left.x, Eq(geom::X{aux_rect.left}));
                EXPECT_THAT(actual_rect.top_left.y, Eq(geom::Y{aux_rect.top}));
                EXPECT_THAT(actual_rect.size.width, Eq(geom::Width{aux_rect.width}));
                EXPECT_THAT(actual_rect.size.height, Eq(geom::Height{aux_rect.height}));
            }

            EXPECT_TRUE(params.placement_hints.is_set());
            if (params.placement_hints.is_set())
                EXPECT_THAT(params.placement_hints.value(), Eq(placement_hints));

            EXPECT_TRUE(params.surface_placement_gravity.is_set());
            if (params.surface_placement_gravity.is_set())
                EXPECT_THAT(params.surface_placement_gravity.value(), Eq(surface_gravity));

            EXPECT_TRUE(params.aux_rect_placement_gravity.is_set());
            if (params.aux_rect_placement_gravity.is_set())
                EXPECT_THAT(params.aux_rect_placement_gravity.value(), Eq(rect_gravity));

            EXPECT_TRUE(params.aux_rect_placement_offset_x.is_set());
            if (params.aux_rect_placement_offset_x.is_set())
                EXPECT_THAT(params.aux_rect_placement_offset_x.value(), Eq(offset_dx));

            EXPECT_TRUE(params.aux_rect_placement_offset_y.is_set());
            if (params.aux_rect_placement_offset_y.is_set())
                EXPECT_THAT(params.aux_rect_placement_offset_y.value(), Eq(offset_dy));

            received.raise();
            return build(session, params);
        };

    EXPECT_CALL(window_manager, add_surface(_,_,_)).WillOnce(Invoke(check_placement));

    auto child = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    EXPECT_TRUE(received.wait_for(400ms));

    mir_surface_release_sync(child);
    mir_surface_release_sync(parent);
    mir_connection_release(connection);
}

TEST_F(CustomWindowManagement, when_the_client_places_an_existing_surface_the_request_reaches_the_window_manager)
{
    int const width{800};
    int const height{600};
    MirPixelFormat const format{mir_pixel_format_bgr_888};
    MirRectangle dummy_rect{13, 17, 19, 23};
    MirRectangle const aux_rect{42, 15, 24, 7};
    auto const rect_gravity = mir_placement_gravity_north;
    auto const surface_gravity = mir_placement_gravity_south;
    auto const placement_hints = MirPlacementHints(mir_placement_hints_flip_y|mir_placement_hints_antipodes);
    auto const offset_dx = 5;
    auto const offset_dy = 7;

    start_server();
    auto connection = mir_connect_sync(new_connection().c_str(), __PRETTY_FUNCTION__);
    auto surface_spec = mir_specify_window(connection, width, height, format);
    auto parent = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    surface_spec = mir_specify_menu(
        connection, width, height, format, parent, &dummy_rect, mir_edge_attachment_any);
    auto child = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    surface_spec = mir_create_spec(connection);
    mir_surface_spec_set_placement(
        surface_spec, &aux_rect, rect_gravity, surface_gravity, placement_hints, offset_dx, offset_dy);

    mt::Signal received;

    auto const check_placement = [&](
        std::shared_ptr<ms::Session> const&,
        std::shared_ptr<ms::Surface> const&,
        msh::SurfaceSpecification const& spec)
        {
            EXPECT_TRUE(spec.aux_rect.is_set());
            if (spec.aux_rect.is_set())
            {
                auto const actual_rect = spec.aux_rect.value();
                EXPECT_THAT(actual_rect.top_left.x, Eq(geom::X{aux_rect.left}));
                EXPECT_THAT(actual_rect.top_left.y, Eq(geom::Y{aux_rect.top}));
                EXPECT_THAT(actual_rect.size.width, Eq(geom::Width{aux_rect.width}));
                EXPECT_THAT(actual_rect.size.height, Eq(geom::Height{aux_rect.height}));
            }

            EXPECT_TRUE(spec.placement_hints.is_set());
            if (spec.placement_hints.is_set())
                EXPECT_THAT(spec.placement_hints.value(), Eq(placement_hints));

            EXPECT_TRUE(spec.surface_placement_gravity.is_set());
            if (spec.surface_placement_gravity.is_set())
                EXPECT_THAT(spec.surface_placement_gravity.value(), Eq(surface_gravity));

            EXPECT_TRUE(spec.aux_rect_placement_gravity.is_set());
            if (spec.aux_rect_placement_gravity.is_set())
                EXPECT_THAT(spec.aux_rect_placement_gravity.value(), Eq(rect_gravity));

            EXPECT_TRUE(spec.aux_rect_placement_offset_x.is_set());
            if (spec.aux_rect_placement_offset_x.is_set())
                EXPECT_THAT(spec.aux_rect_placement_offset_x.value(), Eq(offset_dx));

            EXPECT_TRUE(spec.aux_rect_placement_offset_y.is_set());
            if (spec.aux_rect_placement_offset_y.is_set())
                EXPECT_THAT(spec.aux_rect_placement_offset_y.value(), Eq(offset_dy));

            received.raise();
        };

    EXPECT_CALL(window_manager, modify_surface(_,_,_)).WillOnce(Invoke(check_placement));
    mir_surface_apply_spec(child, surface_spec);
    mir_surface_spec_release(surface_spec);

    EXPECT_TRUE(received.wait_for(400ms));

    mir_surface_release_sync(child);
    mir_surface_release_sync(parent);
    mir_connection_release(connection);
}

namespace
{
struct PlacementCheck
{
    PlacementCheck(MirRectangle const& placement) : expected_rect{placement} {}

    void check(MirSurfacePlacementEvent const* placement_event)
    {
        EXPECT_THAT(mir_surface_placement_get_relative_position(placement_event).top, Eq(expected_rect.top));
        EXPECT_THAT(mir_surface_placement_get_relative_position(placement_event).left, Eq(expected_rect.left));
        EXPECT_THAT(mir_surface_placement_get_relative_position(placement_event).height, Eq(expected_rect.height));
        EXPECT_THAT(mir_surface_placement_get_relative_position(placement_event).width, Eq(expected_rect.width));

        received.raise();
    }

    ~PlacementCheck()
    {
        EXPECT_TRUE(received.wait_for(400ms));
    }

private:
    MirRectangle const& expected_rect;
    mt::Signal received;
};

void surface_placement_event_callback(MirSurface* /*surface*/, MirEvent const* event, void* context)
{
    if (mir_event_get_type(event) == mir_event_type_surface_placement)
    {
        auto const placement_event = mir_event_get_surface_placement_event(event);
        static_cast<PlacementCheck*>(context)->check(placement_event);
    }
}
}

TEST_F(CustomWindowManagement, when_the_window_manager_places_a_surface_the_notification_reaches_the_client)
{
    int const width{800};
    int const height{600};
    MirPixelFormat const format{mir_pixel_format_bgr_888};
    MirRectangle placement{42, 15, 24, 7};
    geom::Rectangle placement_{{42, 15}, {24, 7}};

    std::shared_ptr<ms::Surface> scene_surface;

    auto capture_scene_surface = [&](
        std::shared_ptr<ms::Session> const& session,
        ms::SurfaceCreationParameters const& params,
        std::function<mf::SurfaceId(std::shared_ptr<ms::Session> const& session, ms::SurfaceCreationParameters const& params)> const& build)
        ->  mf::SurfaceId
        {
            auto const result = build(session, params);
            scene_surface = session->surface(result);
            return result;
        };

    EXPECT_CALL(window_manager, add_surface(_,_,_)).WillOnce(Invoke(capture_scene_surface));

    start_server();
    auto connection = mir_connect_sync(new_connection().c_str(), __PRETTY_FUNCTION__);

    PlacementCheck placement_check{placement};
    auto surface_spec = mir_specify_window(connection, width, height, format);
    mir_surface_spec_set_event_handler(surface_spec, &surface_placement_event_callback, &placement_check);
    auto surface = mir_surface_create_sync(surface_spec);
    mir_surface_spec_release(surface_spec);

    scene_surface->placed_relative(placement_);

    mir_surface_release_sync(surface);
    mir_connection_release(connection);
}

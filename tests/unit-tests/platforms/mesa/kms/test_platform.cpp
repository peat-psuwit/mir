/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alexandros Frantzis <alexandros.frantzis@canonical.com>
 */

#include "mir/graphics/platform_ipc_package.h"
#include "mir/graphics/event_handler_register.h"
#include "mir/graphics/platform_ipc_operations.h"
#include "mir/graphics/platform_operation_message.h"
#include "src/platforms/mesa/server/kms/platform.h"
#include "src/server/report/null_report_factory.h"
#include "mir/shared_library.h"
#include "mir/options/program_option.h"

#include "mir/test/doubles/mock_buffer.h"
#include "mir/test/doubles/mock_buffer_ipc_message.h"
#include "mir/test/doubles/mock_console_services.h"
#include "mir/test/doubles/null_console_services.h"
#include "mir/test/doubles/null_emergency_cleanup.h"

#include <gtest/gtest.h>

#include "mir_test_framework/udev_environment.h"
#include "mir_test_framework/executable_path.h"
#include "mir/test/pipe.h"

#include "mir/test/doubles/mock_drm.h"
#include "mir/test/doubles/mock_gbm.h"
#include "mir/test/doubles/mock_egl.h"
#include "mir/test/doubles/mock_gl.h"
#include "mir/test/doubles/fd_matcher.h"
#include "mir/test/doubles/stub_console_services.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>

namespace mg = mir::graphics;
namespace mgm = mir::graphics::mesa;
namespace mtd = mir::test::doubles;
namespace mtf = mir_test_framework;

namespace
{

const char probe_platform[] = "probe_graphics_platform";

class MesaGraphicsPlatform : public ::testing::Test
{
public:
    void SetUp()
    {
        using namespace testing;
        Mock::VerifyAndClearExpectations(&mock_drm);
        Mock::VerifyAndClearExpectations(&mock_gbm);
        ON_CALL(mock_egl, eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS))
            .WillByDefault(Return("EGL_AN_extension_string EGL_EXT_platform_base EGL_KHR_platform_gbm"));
        ON_CALL(mock_egl, eglGetDisplay(_))
            .WillByDefault(Return(fake_display));
        ON_CALL(mock_gl, glGetString(GL_RENDERER))
            .WillByDefault(
                Return(
                    reinterpret_cast<GLubyte const*>("GeForce GTX 1070/PCIe/SSE2")));
        ON_CALL(mock_egl, eglGetConfigAttrib(_, _, EGL_NATIVE_VISUAL_ID, _))
            .WillByDefault(
                DoAll(
                    SetArgPointee<3>(GBM_FORMAT_XRGB8888),
                    Return(EGL_TRUE)));
        fake_devices.add_standard_device("standard-drm-devices");
    }

    std::shared_ptr<mgm::Platform> create_platform()
    {
        return std::make_shared<mgm::Platform>(
                mir::report::null_display_report(),
                std::make_shared<mtd::StubConsoleServices>(),
                *std::make_shared<mtd::NullEmergencyCleanup>(),
                mgm::BypassOption::allowed);
    }

    EGLDisplay fake_display{reinterpret_cast<EGLDisplay>(0xabcd)};
    ::testing::NiceMock<mtd::MockDRM> mock_drm;
    ::testing::NiceMock<mtd::MockGBM> mock_gbm;
    ::testing::NiceMock<mtd::MockEGL> mock_egl;
    ::testing::NiceMock<mtd::MockGL> mock_gl;
    mtf::UdevEnvironment fake_devices;
};
}

TEST_F(MesaGraphicsPlatform, connection_ipc_package)
{
    using namespace testing;
    mir::test::Pipe auth_pipe;
    int const auth_fd{auth_pipe.read_fd()};

    /* First time for master DRM fd, second for authenticated fd */
    EXPECT_CALL(mock_drm, open(StrEq("/dev/dri/card0"),_,_));
    EXPECT_CALL(mock_drm, open(StrEq("/dev/dri/card1"),_,_));

    EXPECT_CALL(mock_drm, drmOpen(_,_))
        .WillOnce(Return(auth_fd));

    /* Expect proper authorization */
    EXPECT_CALL(mock_drm, drmGetMagic(auth_fd,_));
    EXPECT_CALL(mock_drm, drmAuthMagic(mtd::IsFdOfDevice("/dev/dri/card0"),_));

    EXPECT_NO_THROW (
        auto platform = create_platform();
        auto ipc_ops = platform->make_ipc_operations();
        auto pkg = ipc_ops->connection_ipc_package();

        ASSERT_TRUE(pkg.get());
        ASSERT_EQ(std::vector<int32_t>::size_type{1}, pkg->ipc_fds.size());
        ASSERT_EQ(auth_fd, pkg->ipc_fds[0]);
    );
}

TEST_F(MesaGraphicsPlatform, a_failure_while_creating_a_platform_results_in_an_error)
{
    using namespace ::testing;

    EXPECT_CALL(mock_drm, open(_,_,_))
            .WillRepeatedly(Return(-1));

    try
    {
        auto platform = create_platform();
    } catch(...)
    {
        return;
    }

    FAIL() << "Expected an exception to be thrown.";
}

TEST_F(MesaGraphicsPlatform, egl_native_display_is_gbm_device)
{
    auto platform = create_platform();
    EXPECT_EQ(mock_gbm.fake_gbm.device, platform->egl_native_display());
}

TEST_F(MesaGraphicsPlatform, probe_returns_unsupported_when_no_drm_udev_devices)
{
    mtf::UdevEnvironment udev_environment;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::unsupported, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_best_when_master)
{
    mtf::UdevEnvironment udev_environment;
    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    udev_environment.add_standard_device("standard-drm-devices");

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::best, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_supported_on_llvmpipe)
{
    mtf::UdevEnvironment udev_environment;
    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    udev_environment.add_standard_device("standard-drm-devices");

    ON_CALL(mock_gl, glGetString(GL_RENDERER))
        .WillByDefault(
            testing::Return(
                reinterpret_cast<GLubyte const*>("llvmpipe (you know, some version)")));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::supported, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_unsupported_when_egl_client_extensions_not_supported)
{
    using namespace testing;

    mtf::UdevEnvironment udev_environment;
    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    udev_environment.add_standard_device("standard-drm-devices");

    ON_CALL(mock_egl, eglQueryString(EGL_NO_DISPLAY, _))
        .WillByDefault(Return(nullptr));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::unsupported, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_supported_when_old_egl_mesa_gbm_platform_supported)
{
    using namespace testing;

    mtf::UdevEnvironment udev_environment;
    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    udev_environment.add_standard_device("standard-drm-devices");

    ON_CALL(mock_egl, eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS))
        .WillByDefault(Return("EGL_KHR_not_really_an_extension EGL_MESA_platform_gbm EGL_EXT_master_of_the_house EGL_EXT_platform_base"));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::best, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_unsupported_when_gbm_platform_not_supported)
{
    using namespace testing;

    mtf::UdevEnvironment udev_environment;
    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    udev_environment.add_standard_device("standard-drm-devices");

    ON_CALL(mock_egl, eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS))
        .WillByDefault(Return("EGL_KHR_not_really_an_extension EGL_EXT_platform_base"));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::unsupported, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_unsupported_when_modesetting_is_not_supported)
{
    using namespace testing;

    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    ON_CALL(mock_drm, drmCheckModesettingSupported(_)).WillByDefault(Return(-ENOSYS));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::unsupported, probe(stub_vt, options));
}

TEST_F(MesaGraphicsPlatform, probe_returns_supported_when_cannot_determine_kms_support)
{
    using namespace testing;

    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    ON_CALL(mock_drm, drmCheckModesettingSupported(_)).WillByDefault(Return(-EINVAL));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::supported, probe(stub_vt, options));

}

TEST_F(MesaGraphicsPlatform, probe_returns_supported_when_unexpected_error_returned)
{
    using namespace testing;

    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    ON_CALL(mock_drm, drmCheckModesettingSupported(_)).WillByDefault(Return(-ENOBUFS));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::supported, probe(stub_vt, options));

}

TEST_F(MesaGraphicsPlatform, probe_returns_supported_when_cannot_determine_busid)
{
    using namespace testing;

    boost::program_options::options_description po;
    mir::options::ProgramOption options;
    auto const stub_vt = std::make_shared<mtd::StubConsoleServices>();

    ON_CALL(mock_drm, drmGetBusid(_)).WillByDefault(Return(nullptr));

    mir::SharedLibrary platform_lib{mtf::server_platform("graphics-mesa-kms")};
    auto probe = platform_lib.load_function<mg::PlatformProbe>(probe_platform);
    EXPECT_EQ(mg::PlatformPriority::supported, probe(stub_vt, options));

}

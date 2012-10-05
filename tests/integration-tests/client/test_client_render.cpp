/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
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
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "mir/process/process.h"

#include "mir/graphics/android/android_buffer.h"

#include <gmock/gmock.h>

namespace mp=mir::process;

struct TestClient
{

/* client code */
static int main_function()
{
    /* only use C api */

    /* make surface */
    /* grab a buffer*/
    /* render pattern */
    /* release */

    return 0;
}

static int exit_function()
{
    return 0;
}

};

struct MockServerGenerator : public MockServerTool
{
    MockServerPackageGenerator(BufferIPCPackage)
    {

    }


    BufferIPCPackage package;
};


struct TestClientIPCRender : public testing::Test
{
    void SetUp() {
        int err;
        const hw_module_t    *hw_module;
        err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hw_module);
        if (err < 0)
            throw std::runtime_error("Could not open hardware module");
        auto alloc_device = std::shared_ptr<struct alloc_device_t> ( hw_module, mir::EmptyDeleter());

        auto alloc_adaptor = std::make_shared<AndroidAllocAdaptor>(alloc_device);

        auto android_buffer = std::make_shared<AndroidBuffer>(alloc_adaptor, size, pf);

        auto package = android_buffer->get_ipc_package();

        mock_server = std::make_shared<mt::MockServerGenerator>();
        test_server = std::make_shared<mt::TestServer>("./test_socket_surface", mock_server_tool);
        test_server->comm.start();

    }

    void TearDown()
    {
        test_server->comm.stop();
    }

    std::shared_ptr<mt::TestServer> test_server;

    std::shared_ptr<MockIPCServer> mock_server; 
};

TEST_F(TestClientIPCRender, test_render)
{
    /* start server */
    auto p = mp::fork_and_run_in_a_different_process(
        TestClient::main_function,
        TestClient::exit_function);

    /* wait for connect */    
    /* wait for buffer sent back */

    EXPECT_TRUE(p->wait_for_termination().succeeded());


    /* verify pattern */
}

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
 * Authored by: Thomas Voss <thomas.voss@canonical.com>
 */

#ifndef MIR_FRONTEND_APPLICATION_MANAGER_H_
#define MIR_FRONTEND_APPLICATION_MANAGER_H_

#include <memory>
#include "mir/frontend/application_session_factory.h"

namespace mir
{

namespace surfaces
{
class ApplicationSurfaceOrganiser;
}

namespace frontend
{

class Session;
class SessionContainer;
class ApplicationFocusSelectionStrategy;
class Focus;

class SessionManager : public SessionStore
{
 public:
    explicit SessionManager(std::shared_ptr<surfaces::ApplicationSurfaceOrganiser> const& surface_organiser,
                                std::shared_ptr<SessionContainer> const& session_container,
                                std::shared_ptr<ApplicationFocusSelectionStrategy> const& focus_selection_strategy,
                                std::shared_ptr<Focus> const& focus_mechanism);
    virtual ~SessionManager() {}

    virtual std::shared_ptr<Session> open_session(std::string const& name);
    virtual void close_session(std::shared_ptr<Session> const& session);

    void focus_next();

protected:
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

private:
    std::shared_ptr<surfaces::ApplicationSurfaceOrganiser> surface_organiser;
    std::shared_ptr<SessionContainer> app_container;
    std::shared_ptr<ApplicationFocusSelectionStrategy> focus_selection_strategy;
    std::shared_ptr<Focus> focus_mechanism;

    std::weak_ptr<Session> focus_application;
};

}
}

#endif // MIR_FRONTEND_APPLICATION_MANAGER_H_

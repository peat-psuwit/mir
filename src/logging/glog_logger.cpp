/*
 * Copyright © 2013 Canonical Ltd.
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
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "mir/logging/glog_logger.h"

#include <glog/logging.h>

void mir::logging::GlogLogger::log(Severity severity, const std::string& message, const std::string& component)
{
    static int glog_level[] =
    {
        google::GLOG_FATAL, //critical = 0,
        google::GLOG_ERROR, //error = 1,
        google::GLOG_WARNING, //warning = 2,
        google::GLOG_INFO, //informational = 3,
        google::GLOG_INFO, //debug = 4
    };

    // Since we're not collecting __FILE__ or __LINE__ this is misleading
    google::LogMessage(__FILE__, __LINE__, glog_level[severity]).stream()
        << '[' << component << "] " << message;
}

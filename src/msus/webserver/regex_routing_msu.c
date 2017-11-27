/*
START OF LICENSE STUB
    DeDOS: Declarative Dispersion-Oriented Software
    Copyright (C) 2017 University of Pennsylvania

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
END OF LICENSE STUB
*/
#include "msu_message.h"
#include "local_msu.h"
#include "msu_calls.h"

#include "webserver/regex_routing_msu.h"
#include "webserver/regex_msu.h"

static int routing_receive(struct local_msu *self, struct msu_msg *msg) {
    return call_msu_type(self, &WEBSERVER_REGEX_MSU_TYPE, &msg->hdr, msg->data_size, msg->data);
}

struct msu_type WEBSERVER_REGEX_ROUTING_MSU_TYPE = {
    .name = "Regex_Routing_MSU",
    .id = WEBSERVER_REGEX_ROUTING_MSU_TYPE_ID,
    .receive = routing_receive
};

#!/bin/sh
#
# PROVIDE: fand
# REQUIRE: LOGIN
# BEFORE:  securelevel
# KEYWORD: shutdown

#
#fand_enable="YES"
#

. /etc/rc.subr

name="fand"
rcvar=fand_enable

extra_commands="reload"

command="/opt/bin/fand"
#config_file="/usr/local/etc/$name.conf"
#command_args="${config_file}"
pidfile="/var/run/$name.pid"
#required_files="${config_file}"

# read configuration and set defaults
load_rc_config "$name"
: ${fand_enable="NO"}
#: ${fand_user="fand"}

run_rc_command "$1"

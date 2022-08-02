#!/usr/bin/env python2
#
# This script will add a target to the BOINC system, which would normally
# require a bunch of manual work in terms of editing files, and creating
# a whole directory structure.  The gist of it is this:
# - Edit project.xml if the platform is new
# - Edit config.xml to add daemons such as validators and assimilators
# - Create the following directory structure:
#     apps/$TARGET_NAME/$VERSION/$PLATFORM
#   (where $VERSION is currently hard-coded to "1")
# - The contents of skel/$PLATFORM are then copied to the new app directory
#   with the filenames getting the $TARGET_NAME injected into them and
#   version.xml has all instances of {app} replaced with $TARGET_NAME
# - Creates templates/$TARGET_NAME_$PLATFORM_{in,out} based on files in
#   skel/templates. See: https://boinc.berkeley.edu/trac/wiki/JobTemplates
# - Calls xadd, stop, and start to ensure BOINC knows about all these changes
#

import argparse
import fcntl
import os
import os.path
import shutil
import socket
import subprocess
import sys

import boinc_path_config
from Boinc import configxml, projectxml

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('app')
    parser.add_argument('platforms', nargs=argparse.REMAINDER)
    return parser.parse_args()

def add_app(project_file, name, platform):
    app_name = f'{name}_{platform}'
    for node in project_file.elements:
        if node._name == 'app' and node.name == app_name:
            print(f'App {app_name} already in project.xml, not adding app')
            return

    a = project_file.elements.make_node_and_append('app')
    a.name = app_name
    a.user_friendly_name = f'{name} running on {platform}'

def create_app_dir(name, platform):
    app_name = f'{name}_{platform}'
    app_dir = os.path.join('apps', app_name)
    if os.path.exists(app_dir):
        print(f'App directory {app_dir} already exists, not adding app versions')
        return

    app_version_dir = os.path.join(app_dir, '1', platform)
    os.makedirs(app_version_dir)

    skel_dir = os.path.join('skel', platform)
    for filename in os.listdir(skel_dir):
        if filename != 'version.xml':
            name, dot, ext = filename.partition('.')
            new_filename = ''.join((name, '.', app_name, dot, ext))
            shutil.copy(os.path.join(skel_dir, filename),
                        os.path.join(app_version_dir, new_filename))
        else:
            with open(os.path.join(skel_dir, filename)) as template:
                version_xml = template.read().format(app=app_name)
            with open(os.path.join(app_version_dir, filename), 'w') as version_file:
                version_file.write(version_xml)

def create_app_templates(name, platform):
    in_template_path = os.path.join('templates', f'{name}_{platform}_in')
    if os.path.exists(in_template_path):
        print(
            f'Input template {in_template_path} already exists, not adding templates'
        )

        return
    out_template_path = os.path.join('templates', f'{name}_{platform}_out')
    if os.path.exists(out_template_path):
        print(
            f'Output template {out_template_path} already exists, not adding templates'
        )

        return

    shutil.copyfile(
        os.path.join('skel', 'templates', f'{platform}_in'), in_template_path
    )

    shutil.copyfile(
        os.path.join('skel', 'templates', f'{platform}_out'), out_template_path
    )


def add_daemons(config_file, name, platform):
    app_name = f'{name}_{platform}'
    cmd = f'killerbeez_assimilator.py -app {app_name}'

    for node in config_file.daemons:
        if node.cmd == cmd:
            print(f'Assimilator daemon for app {app_name} already exists, not adding it')
            return

    daemon = config_file.daemons.make_node_and_append('daemon')
    daemon.cmd = cmd
    daemon.pid_file = f'killerbeez_assimilator_{app_name}.pid'
    daemon.lock_file = f'killerbeez_assimilator_{app_name}.lock'
    daemon.output = f'killerbeez_assimilator_{app_name}.log'

    daemon = config_file.daemons.make_node_and_append('daemon')
    daemon.cmd = f'sample_trivial_validator --app {app_name}'
    daemon.pid_file = f'sample_trivial_validator_{app_name}.pid'
    daemon.lock_file = f'sample_trivial_validator_{app_name}.lock'
    daemon.output = f'sample_trivial_validator_{app_name}.log'

def lock_file(filename):
    os.umask(02)
    file = open(filename,'w')
    fcntl.lockf(file.fileno(), fcntl.LOCK_EX|fcntl.LOCK_NB)


def main():
    args = parse_args()
    if not os.path.isfile('project.xml'):
        print('Must be run from the project directory')
        sys.exit(1)

    hostname = socket.gethostname().split('.')[0]
    lockfile_name = os.path.join(f'pid_{hostname}', 'add_target.lock')
    try:
        lock_file(lockfile_name)
    except IOError:
        print(f'Another {sys.argv[0]} process is running, please try again')

    project_file = projectxml.ProjectFile('project.xml').read()
    config_file = configxml.ConfigFile('config.xml').read()

    name = args.app
    platforms = args.platforms

    for platform in platforms:
        # Add name_platform to apps in project.xml
        add_app(project_file, name, platform)
        # Create app directory with wrapper, version.xml
        create_app_dir(name, platform)
        create_app_templates(name, platform)
        add_daemons(config_file, name, platform)

    project_file.write()
    config_file.write()
    os.unlink(lockfile_name)

    # Update db from project file
    subprocess.check_call(['bin/xadd'])
    # Restart project
    subprocess.check_call(['bin/stop'])
    subprocess.check_call(['bin/start'])

    print(
        f'New app versions installed into apps/{name}_*. Make any changes you need, then run bin/update_versions to install them.'
    )

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Pre-build script to ensure config.ini exists.
Copies config.ini.template to config.ini if it doesn't exist.
"""
import os
import shutil
import sys

# Try to get environment from PlatformIO
try:
    Import("env")
    project_dir = env.get("PROJECT_DIR")
except:
    # Running standalone
    project_dir = os.path.dirname(os.path.abspath(sys.argv[0])) if '__file__' not in dir() else os.path.dirname(os.path.abspath(__file__))

config_file = os.path.join(project_dir, "config.ini")
template_file = os.path.join(project_dir, "config.ini.template")

if not os.path.exists(config_file):
    if os.path.exists(template_file):
        print("Creating config.ini from template...")
        shutil.copy(template_file, config_file)
        print("✓ config.ini created. Please customize it for your environment.")
    else:
        print("ERROR: config.ini.template not found!")
        print("Cannot proceed without configuration template.")
        sys.exit(1)


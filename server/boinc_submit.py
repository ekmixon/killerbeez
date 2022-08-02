#!/usr/bin/env python

import base64
import collections

import requests

# Set the following to configure your job
PROJECT = 'http://localhost:5000/api'
SEED = b"1234seed"

def main():
    # Make sure we have a DB entry for the target
    target_resp = requests.post(
        f'{PROJECT}/target',
        json={"platform": "windows_x86_64", "target_executable": "wmp"},
    )

    target_resp.raise_for_status()
    target_id = target_resp.json()['id']
    # Set up shell format for this target - change value to "sh" for linux
    requests.post(
        f'{PROJECT}/config',
        json={
            "target_id": target_id,
            "name": "platform_opts_shell_format",
            "value": "bat",
        },
    ).raise_for_status()


    # Create the driver and instrumentation configs
    requests.post(
        f'{PROJECT}/config',
        json={
            "target_id": target_id,
            "name": "driver_opts_wmp",
            "value": r'{"path": "C:\\Program Files\\Windows Media Player\\wmplayer.exe"}',
        },
    ).raise_for_status()

    requests.post(
        f'{PROJECT}/config',
        json={
            "target_id": target_id,
            "name": "instrumentation_opts_dynamorio",
            "value": r'{"per_module_coverage": 1, "timeout": 10000, "coverage_modules": ["wmp.DLL"], "client_params": "-target_module wmplayer.exe -target_offset 0x1F20 -nargs 3", "fuzz_iterations": 1, "target_path": "C:\\Program Files\\Windows Media Player\\wmplayer.exe"}',
        },
    ).raise_for_status()


    # Create the seed file
    seed_resp = requests.post(
        f'{PROJECT}/file',
        json={
            "content": base64.b64encode(SEED).decode(),
            "encoding": "base64",
        },
    )

    seed_resp.raise_for_status()
    seed_file = seed_resp.json()['filename']

    # Create the job!
    job_resp = requests.post(
        f'{PROJECT}/job',
        json={
            "job_type": "fuzz",
            "target_id": target_id,
            "mutator": "radamsa",
            "instrumentation_type": "dynamorio",
            "driver": "wmp",
            "seed_file": seed_file,
            "iterations": 2,
        },
    )

    job_resp.raise_for_status()
    job_json = job_resp.json()

    print(f"Created job {job_json['job_id']} with BOINC id {job_json['boinc_id']}")


if __name__ == '__main__':
    main()

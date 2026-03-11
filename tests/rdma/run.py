#!/usr/bin/python3
"""
==========================================================================
run.py - script for test client for Valkey Over RDMA (Linux only)
--------------------------------------------------------------------------
Copyright (C) 2024  zhenwei pi <pizhenwei@bytedance.com>

This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
the top-level directory.
==========================================================================
"""
import os
import subprocess
import netifaces
import time
import argparse


def run_cmd(cmd, timeout, desc):
    start = time.time()
    proc = subprocess.Popen(cmd, shell=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        outs, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        outs, _ = proc.communicate()
        print("Valkey Over RDMA " + desc + " [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        return 1

    if proc.returncode:
        print("Valkey Over RDMA " + desc + " [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        return 1

    elapsed = time.time() - start
    print("Valkey Over RDMA " + desc + " in " + str(round(elapsed, 2)) + "s [OK]")
    if outs:
        print(outs.decode())
    return 0


def build_program():
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
    cmd = "make -C " + valkeydir + "/tests/rdma"
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    if p.wait():
        print("Valkey Over RDMA build rdma-test [FAILED]")
        return 1

    print("Valkey Over RDMA build rdma-test program [OK]")
    return 0


# iterate /sys/class/infiniband, find any usable RDMA device, and return IPv4/IPV6 address
def find_rdma_dev():
    # Ex, /sys/class/infiniband/mlx5_0
    # Ex, /sys/class/infiniband/rxe_eth0
    # Ex, /sys/class/infiniband/siw_eth0
    ibclass = "/sys/class/infiniband/"
    try:
        for dev in os.listdir(ibclass):
            # Ex, /sys/class/infiniband/rxe_eth0/ports/1/gid_attrs/ndevs/0
            netdev = ibclass + dev + "/ports/1/gid_attrs/ndevs/0"
            with open(netdev) as fp:
                addrs = netifaces.ifaddresses(fp.readline().strip("\n"))
                if netifaces.AF_INET in addrs:
                    ipaddr = addrs[netifaces.AF_INET][0]["addr"]
                elif netifaces.AF_INET6 in addrs:
                    ipaddr = addrs[netifaces.AF_INET6][0]["addr"]
                else:
                    continue
                print("Valkey Over RDMA test prepare " + dev + " <" + ipaddr  + "> [OK]")
                return ipaddr
    except os.error:
        return None

    return None


def test_rdma(ipaddr):
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
    retval = 0

    # step 1, prepare test directory
    tmpdir = valkeydir + "/tests/rdma/tmp"
    subprocess.Popen("mkdir -p " + tmpdir, shell=True).wait()

    # step 2, start server
    svrpath = valkeydir + "/src/valkey-server"
    svrcmd = [svrpath, "--port", "0", "--loglevel", "verbose", "--protected-mode", "yes",
             "--appendonly", "no", "--daemonize", "no", "--dir", valkeydir + "/tests/rdma/tmp",
             "--rdma-port", "6379", "--rdma-bind", ipaddr]

    svr = subprocess.Popen(svrcmd, shell=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if svr.wait(1):
             print("Valkey Over RDMA valkey-server runs less than 1s [FAILED]")
             return 1
    except subprocess.TimeoutExpired as e:
        print("Valkey Over RDMA valkey-server start [OK]")
        pass

    # step 3, run test client
    clipath = valkeydir + "/tests/rdma/rdma-test"
    clicmd = [clipath, "--thread", "4", "-h", ipaddr]
    retval = run_cmd(clicmd, 60, "test")

    if retval == 0:
        benchpath = valkeydir + "/src/valkey-benchmark"
        setcmd = [benchpath, "--rdma", "-h", ipaddr,
                  "-r", "64", "--sequential", "-c", "1", "-n", "64",
                  "-d", "262144", "-t", "set", "-q"]
        retval = run_cmd(setcmd, 60, "benchmark(set)")

    if retval == 0:
        getcmd = [benchpath, "--rdma", "-h", ipaddr,
                  "-r", "64", "-c", "16", "-n", "512", "-t", "get", "-q"]
        retval = run_cmd(getcmd, 60, "benchmark(get)")

    # step 4, cleanup
    svr.kill()
    svr.wait()
    subprocess.Popen("rm -rf " + tmpdir, shell=True).wait()

    # step 5, report result
    return retval


def test_exit(retval, install_rxe):
    if install_rxe and not os.geteuid():
        rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
        cmd = rdma_env_py + " -o cleanup"
        subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE).wait()

    os._exit(retval);


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description = "Script to test Valkey Over RDMA",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-r", "--install-rxe", action='store_true',
        help="install RXE driver and setup RXE device")
    args = parser.parse_args()

    if args.install_rxe:
        if os.geteuid():
            print("--install-rxe/-r must be root privileged")
            test_exit(1, False)

        rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
        cmd = rdma_env_py + " -o setup -d rxe"
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
        if p.wait():
            print("Valkey Over RDMA setup RXE [FAILED]")
            test_exit(1, False)

    # build C client into binary
    retval = build_program()
    if retval:
        test_exit(1, args.install_rxe)

    ipaddr = find_rdma_dev()
    if ipaddr is None:
        # not fatal error, continue to create software version: RXE and SIW
        print("Valkey Over RDMA test detect existing RDMA device [FAILED]")
    else:
        retval = test_rdma(ipaddr)
        if not retval:
            print("Valkey Over RDMA test over " + ipaddr + " [OK]")

    test_exit(0, args.install_rxe);

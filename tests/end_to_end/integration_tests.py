#!/usr/bin/python3
import json
import os
import os.path
import subprocess
import sys
import multiprocessing

mt_kahypar_dir = os.environ.get("HOME") + "/mt-kahypar/"
executable_dir = mt_kahypar_dir + "build/mt-kahypar/application/"
config_dir = mt_kahypar_dir + "config/"
integration_test_json_file = mt_kahypar_dir + "tests/end_to_end/integration_tests.json"
num_threads = multiprocessing.cpu_count()


partitioners = { "Mt-KaHyPar-D":     { "executable": executable_dir + "MtKaHyParFast",
                                       "config":  config_dir + "default_preset.ini" },
                 "Mt-KaHyPar-Q":     { "executable": executable_dir + "MtKaHyParStrong",
                                       "config":  config_dir + "quality_preset.ini" },
                 "Mt-KaHyPar-Graph": { "executable": executable_dir + "MtKaHyParGraph",
                                       "config":  config_dir + "default_preset.ini" },
                 "Mt-KaHyPar-Det":   { "executable": executable_dir + "MtKaHyParFast",
                                       "config":  config_dir + "speed_deterministic_preset.ini" } }

def bold(msg):
  return "\033[1m" + msg + "\033[0m"

def print_error(msg):
  print("\033[1;91m[ERROR]\033[0m " + bold(msg))

def print_success(msg):
  print("\033[1;92m[SUCCESS]\033[0m " + bold(msg))

def print_config(instance, k, epsilon):
  print(bold("Instance = " + instance + ", k = " + str(k) + ", Epsilon = " + str(epsilon)))

def command(test, instance, k, epsilon):
  partitioner = partitioners[test["partitioner"]]
  config = config_dir + test["config"] if "config" in test else partitioner["config"]
  parameters = test["parameters"] if "parameters" in test else []
  return [ partitioner["executable"],
         "-h" + instance,
         "-p" + config,
         "-k" + str(k),
         "-e" + str(epsilon),
         "-t" + str(num_threads),
         "-okm1",
         "-mdirect",
         "--show-detailed-timings=true",
         "--sp-process=true"] + parameters

def grep_result(out):
  for line in out.split('\n'):
    s = str(line).strip()
    if "RESULT" in s:
      km1 = int(s.split(" km1=")[1].split(" ")[0])
      cut = int(s.split(" cut=")[1].split(" ")[0])
      total_time = float(s.split(" totalPartitionTime=")[1].split(" ")[0])
      imbalance = float(s.split(" imbalance=")[1].split(" ")[0])
  return "Total Time = " + str(total_time) + \
         " Imbalance = " + str(imbalance) + \
         " km1 = " + str(km1) + \
         " cut = " + str(cut)

def run_integration_test(cmd):
  proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, universal_newlines=True)
  out, err = proc.communicate()

  if proc.returncode == 0:
    print_success(grep_result(out))
  else:
    print_error("Partitioner terminates with non-zero exit code (Exit Code = " + str(proc.returncode) + ")")
    print(out)
    sys.exit(-1)




with open(integration_test_json_file) as integration_test_file:
  integration_tests = json.load(integration_test_file)
  epsilon = integration_tests["epsilon"]
  for instance in integration_tests["instances"]:
    absoulute_instance_path = mt_kahypar_dir + instance
    for k in integration_tests["k"]:
      print_config(instance, k, epsilon)
      for test in integration_tests["tests"]:
        cmd = command(test, absoulute_instance_path, k, epsilon)
        print(' '.join(cmd))
        run_integration_test(cmd)
      print()




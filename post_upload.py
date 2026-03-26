Import("env")
import subprocess, os, sys, time

def after_upload(source, target, env):
    boot_dir = os.path.join(env.subst("$PROJECT_DIR"), "bootloader")
    print("\n>>> Waiting for STLink to re-enumerate... <<<")
    time.sleep(4)
    print(">>> Re-flashing bootloader to preserve pages 0-3 <<<")
    subprocess.run([sys.executable, "-m", "platformio", "run",
                    "-d", boot_dir, "-t", "upload"])

env.AddPostAction("upload", after_upload)

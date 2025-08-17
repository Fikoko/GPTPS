

import pytest
from pathlib import Path
import shutil
import json
from src.work_process_types.pre_process.load_config import detect_hardware, load_hardware_patterns, apply_hardware_pattern, create_resource_subdirs, main, GLOBAL_CONFIG_PATH, RESOURCE_DIR

# Temporary test folders
TEST_RESOURCE_DIR = RESOURCE_DIR / "test_resource"

@pytest.fixture(scope="module")
def setup_resource():
    # Ensure clean test folder
    if TEST_RESOURCE_DIR.exists():
        shutil.rmtree(TEST_RESOURCE_DIR)
    TEST_RESOURCE_DIR.mkdir(parents=True, exist_ok=True)
    yield TEST_RESOURCE_DIR
    # Cleanup after test
    shutil.rmtree(TEST_RESOURCE_DIR)

def test_detect_hardware():
    hw = detect_hardware()
    assert "cpu" in hw
    assert "ram_gb" in hw
    assert "gpu" in hw
    assert isinstance(hw["ram_gb"], int)
    assert isinstance(hw["gpu"], list)

def test_load_and_apply_patterns():
    patterns = load_hardware_patterns()
    hw = detect_hardware()
    defaults = apply_hardware_pattern(hw, patterns)
    # Should return a dictionary
    assert isinstance(defaults, dict)
    assert "tasks" in defaults or defaults == {}

def test_create_resource_subdirs(setup_resource):
    subdirs = ["input_test", "output_test"]
    paths = [setup_resource / sd for sd in subdirs]
    create_resource_subdirs(paths)
    for p in paths:
        assert p.exists()
        assert p.is_dir()

def test_main_autofill(monkeypatch, setup_resource):
    # Patch RESOURCE_DIR to test folder
    monkeypatch.setattr("src.work_process_types.pre_process.load_config.RESOURCE_DIR", setup_resource)
    config = main(auto_fill=True, interactive=False)
    # Assert config keys exist
    for key in ["os", "hardware", "tasks", "globals", "file_handling", "performance"]:
        assert key in config
    # Assert workspace paths were created
    for k, v in config["globals"].items():
        if "workspace" in k:
            p = Path(v)
            assert p.exists()
            assert p.is_dir()
    # Assert global_config.json file exists
    assert GLOBAL_CONFIG_PATH.exists()
    # Cleanup generated config
    if GLOBAL_CONFIG_PATH.exists():
        GLOBAL_CONFIG_PATH.unlink()

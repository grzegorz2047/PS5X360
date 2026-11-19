"""Fixtures for the binary patch tests.

The tests need the official v1.0.2 executable, which is not in the repository:

    PS5X360_BASE_ZIP=/path/to/PPSA50011.zip   (the official v1.0.2 release zip), or
    PS5X360_BASE_ELF=/path/to/eboot.elf       (the ELF already taken out of its eboot.bin)

Without either, every test that needs it is skipped (CI sets PS5X360_BASE_ZIP).
"""
import os

import pytest

import elfpatch


@pytest.fixture(scope="session")
def base():
    zip_path = os.environ.get("PS5X360_BASE_ZIP")
    elf_path = os.environ.get("PS5X360_BASE_ELF")
    if not zip_path and not elf_path:
        pytest.skip("set PS5X360_BASE_ZIP (official v1.0.2 PPSA50011.zip) or PS5X360_BASE_ELF")
    elf, eboot = elfpatch.load_base(zip_path=zip_path, elf_path=None if zip_path else elf_path)
    return {"elf": elf, "eboot": eboot}


@pytest.fixture(scope="session")
def base_elf(base):
    return base["elf"]


@pytest.fixture(scope="session")
def applied(base_elf):
    """(patched ELF, [Patch...])"""
    return elfpatch.record_patches(base_elf)


@pytest.fixture(scope="session")
def patched_elf(applied):
    return applied[0]


@pytest.fixture(scope="session")
def records(applied):
    return applied[1]


@pytest.fixture(scope="session")
def base_image(base_elf):
    return elfpatch.Image(base_elf)


@pytest.fixture(scope="session")
def patched_image(patched_elf):
    return elfpatch.Image(patched_elf)

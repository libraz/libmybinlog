BUILD_DIR ?= build
NPROC ?= 3
CMAKE ?= cmake
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy

CXX_FILES = git ls-files -z -- '*.h' '*.cpp'

.PHONY: build test format format-check lint clean

build:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) -j $(NPROC)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -j $(NPROC)

format:
	$(CXX_FILES) | xargs -0 -r $(CLANG_FORMAT) -i
	$(MAKE) lint

format-check:
	$(CXX_FILES) | xargs -0 -r $(CLANG_FORMAT) --dry-run --Werror
	$(MAKE) lint

# Static analysis only; needs $(BUILD_DIR)/compile_commands.json from `make build`.
lint:
	@test -f $(BUILD_DIR)/compile_commands.json || $(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	git ls-files -z -- 'src/*.cpp' | xargs -0 -r $(CLANG_TIDY) -p $(BUILD_DIR) --quiet

clean:
	rm -rf $(BUILD_DIR)

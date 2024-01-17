# - Find mlx5
# Find the mlx5 library and includes
#
# MLX5_INCLUDE_DIR - where to find mlx5dv.h, etc.
# MLX5_LIBRARIES - List of libraries when using mlx5.
# MLX5_FOUND - True if mlx5 found.

find_path(MLX5_INCLUDE_DIR infiniband/mlx5dv.h)
find_library(MLX5_LIBRARIES mlx5)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(mlx5 DEFAULT_MSG MLX5_LIBRARIES MLX5_INCLUDE_DIR)

if(MLX5_FOUND)
  if(NOT TARGET Provider::MLX5)
    add_library(Provider::MLX5 UNKNOWN IMPORTED)
  endif()
  set_target_properties(Provider::MLX5 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MLX5_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${MLX5_LIBRARIES}")
endif()

mark_as_advanced(
  MLX5_LIBRARIES
)

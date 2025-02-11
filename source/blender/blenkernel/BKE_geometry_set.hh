/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup bke
 */

#include <atomic>
#include <iostream>

#include "BLI_float3.hh"
#include "BLI_float4x4.hh"
#include "BLI_function_ref.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_user_counter.hh"

#include "BKE_attribute_access.hh"
#include "BKE_geometry_set.h"

struct Collection;
struct Mesh;
struct Object;
struct PointCloud;
struct Volume;

enum class GeometryOwnershipType {
  /* The geometry is owned. This implies that it can be changed. */
  Owned = 0,
  /* The geometry can be changed, but someone else is responsible for freeing it. */
  Editable = 1,
  /* The geometry cannot be changed and someone else is responsible for freeing it. */
  ReadOnly = 2,
};

namespace blender::bke {
class ComponentAttributeProviders;
}

class GeometryComponent;

/**
 * Contains information about an attribute in a geometry component.
 * More information can be added in the future. E.g. whether the attribute is builtin and how it is
 * stored (uv map, vertex group, ...).
 */
struct AttributeMetaData {
  AttributeDomain domain;
  CustomDataType data_type;
};

/* Returns false when the iteration should be stopped. */
using AttributeForeachCallback = blender::FunctionRef<bool(blender::StringRefNull attribute_name,
                                                           const AttributeMetaData &meta_data)>;

/**
 * Base class for the attribute intializer types described below.
 */
struct AttributeInit {
  enum class Type {
    Default,
    VArray,
    MoveArray,
  };
  Type type;
  AttributeInit(const Type type) : type(type)
  {
  }
};

/**
 * Create an attribute using the default value for the data type.
 * The default values may depend on the attribute provider implementation.
 */
struct AttributeInitDefault : public AttributeInit {
  AttributeInitDefault() : AttributeInit(Type::Default)
  {
  }
};

/**
 * Create an attribute by copying data from an existing virtual array. The virtual array
 * must have the same type as the newly created attribute.
 *
 * Note that this can be used to fill the new attribute with the default
 */
struct AttributeInitVArray : public AttributeInit {
  const blender::fn::GVArray *varray;

  AttributeInitVArray(const blender::fn::GVArray *varray)
      : AttributeInit(Type::VArray), varray(varray)
  {
  }
};

/**
 * Create an attribute with a by passing ownership of a pre-allocated contiguous array of data.
 * Sometimes data is created before a geometry component is available. In that case, it's
 * preferable to move data directly to the created attribute to avoid a new allocation and a copy.
 *
 * Note that this will only have a benefit for attributes that are stored directly as contiguous
 * arrays, so not for some built-in attributes.
 *
 * The array must be allocated with MEM_*, since `attribute_try_create` will free the array if it
 * can't be used directly, and that is generally how Blender expects custom data to be allocated.
 */
struct AttributeInitMove : public AttributeInit {
  void *data = nullptr;

  AttributeInitMove(void *data) : AttributeInit(Type::MoveArray), data(data)
  {
  }
};

/**
 * This is the base class for specialized geometry component types.
 */
class GeometryComponent {
 private:
  /* The reference count has two purposes. When it becomes zero, the component is freed. When it is
   * larger than one, the component becomes immutable. */
  mutable std::atomic<int> users_ = 1;
  GeometryComponentType type_;

 public:
  GeometryComponent(GeometryComponentType type);
  virtual ~GeometryComponent() = default;
  static GeometryComponent *create(GeometryComponentType component_type);

  /* The returned component should be of the same type as the type this is called on. */
  virtual GeometryComponent *copy() const = 0;

  /* Direct data is everything except for instances of objects/collections.
   * If this returns true, the geometry set can be cached and is still valid after e.g. modifier
   * evaluation ends. Instances can only be valid as long as the data they instance is valid. */
  virtual bool owns_direct_data() const = 0;
  virtual void ensure_owns_direct_data() = 0;

  void user_add() const;
  void user_remove() const;
  bool is_mutable() const;

  GeometryComponentType type() const;

  /* Return true when any attribute with this name exists, including built in attributes. */
  bool attribute_exists(const blender::StringRef attribute_name) const;

  /* Return the data type and domain of an attribute with the given name if it exists. */
  std::optional<AttributeMetaData> attribute_get_meta_data(
      const blender::StringRef attribute_name) const;

  /* Returns true when the geometry component supports this attribute domain. */
  bool attribute_domain_supported(const AttributeDomain domain) const;
  /* Can only be used with supported domain types. */
  virtual int attribute_domain_size(const AttributeDomain domain) const;

  bool attribute_is_builtin(const blender::StringRef attribute_name) const;

  /* Get read-only access to the highest priority attribute with the given name.
   * Returns null if the attribute does not exist. */
  blender::bke::ReadAttributeLookup attribute_try_get_for_read(
      const blender::StringRef attribute_name) const;

  /* Get read and write access to the highest priority attribute with the given name.
   * Returns null if the attribute does not exist. */
  blender::bke::WriteAttributeLookup attribute_try_get_for_write(
      const blender::StringRef attribute_name);

  /* Get a read-only attribute for the domain based on the given attribute. This can be used to
   * interpolate from one domain to another.
   * Returns null if the interpolation is not implemented. */
  virtual std::unique_ptr<blender::fn::GVArray> attribute_try_adapt_domain(
      std::unique_ptr<blender::fn::GVArray> varray,
      const AttributeDomain from_domain,
      const AttributeDomain to_domain) const;

  /* Returns true when the attribute has been deleted. */
  bool attribute_try_delete(const blender::StringRef attribute_name);

  /* Returns true when the attribute has been created. */
  bool attribute_try_create(const blender::StringRef attribute_name,
                            const AttributeDomain domain,
                            const CustomDataType data_type,
                            const AttributeInit &initializer);

  /* Try to create the builtin attribute with the given name. No data type or domain has to be
   * provided, because those are fixed for builtin attributes. */
  bool attribute_try_create_builtin(const blender::StringRef attribute_name,
                                    const AttributeInit &initializer);

  blender::Set<std::string> attribute_names() const;
  bool attribute_foreach(const AttributeForeachCallback callback) const;

  virtual bool is_empty() const;

  /* Get a virtual array to read the data of an attribute on the given domain and data type.
   * Returns null when the attribute does not exist or cannot be converted to the requested domain
   * and data type. */
  std::unique_ptr<blender::fn::GVArray> attribute_try_get_for_read(
      const blender::StringRef attribute_name,
      const AttributeDomain domain,
      const CustomDataType data_type) const;

  /* Get a virtual array to read the data of an attribute on the given domain. The data type is
   * left unchanged. Returns null when the attribute does not exist or cannot be adapted to the
   * requested domain. */
  std::unique_ptr<blender::fn::GVArray> attribute_try_get_for_read(
      const blender::StringRef attribute_name, const AttributeDomain domain) const;

  /* Get a virtual array to read data of an attribute with the given data type. The domain is
   * left unchanged. Returns null when the attribute does not exist or cannot be converted to the
   * requested data type. */
  blender::bke::ReadAttributeLookup attribute_try_get_for_read(
      const blender::StringRef attribute_name, const CustomDataType data_type) const;

  /* Get a virtual array to read the data of an attribute. If that is not possible, the returned
   * virtual array will contain a default value. This never returns null. */
  std::unique_ptr<blender::fn::GVArray> attribute_get_for_read(
      const blender::StringRef attribute_name,
      const AttributeDomain domain,
      const CustomDataType data_type,
      const void *default_value = nullptr) const;

  /* Should be used instead of the method above when the requested data type is known at compile
   * time for better type safety. */
  template<typename T>
  blender::fn::GVArray_Typed<T> attribute_get_for_read(const blender::StringRef attribute_name,
                                                       const AttributeDomain domain,
                                                       const T &default_value) const
  {
    const blender::fn::CPPType &cpp_type = blender::fn::CPPType::get<T>();
    const CustomDataType type = blender::bke::cpp_type_to_custom_data_type(cpp_type);
    std::unique_ptr varray = this->attribute_get_for_read(
        attribute_name, domain, type, &default_value);
    return blender::fn::GVArray_Typed<T>(std::move(varray));
  }

  /**
   * Returns an "output attribute", which is essentially a mutable virtual array with some commonly
   * used convince features. The returned output attribute might be empty if requested attribute
   * cannot exist on the geometry.
   *
   * The included convenience features are:
   * - Implicit type conversion when writing to builtin attributes.
   * - If the attribute name exists already, but has a different type/domain, a temporary attribute
   *   is created that will overwrite the existing attribute in the end.
   */
  blender::bke::OutputAttribute attribute_try_get_for_output(
      const blender::StringRef attribute_name,
      const AttributeDomain domain,
      const CustomDataType data_type,
      const void *default_value = nullptr);

  /* Same as attribute_try_get_for_output, but should be used when the original values in the
   * attributes are not read, i.e. the attribute is used only for output. Since values are not read
   * from this attribute, no default value is necessary. */
  blender::bke::OutputAttribute attribute_try_get_for_output_only(
      const blender::StringRef attribute_name,
      const AttributeDomain domain,
      const CustomDataType data_type);

  /* Statically typed method corresponding to the equally named generic one. */
  template<typename T>
  blender::bke::OutputAttribute_Typed<T> attribute_try_get_for_output(
      const blender::StringRef attribute_name, const AttributeDomain domain, const T default_value)
  {
    const blender::fn::CPPType &cpp_type = blender::fn::CPPType::get<T>();
    const CustomDataType data_type = blender::bke::cpp_type_to_custom_data_type(cpp_type);
    return this->attribute_try_get_for_output(attribute_name, domain, data_type, &default_value);
  }

  /* Statically typed method corresponding to the equally named generic one. */
  template<typename T>
  blender::bke::OutputAttribute_Typed<T> attribute_try_get_for_output_only(
      const blender::StringRef attribute_name, const AttributeDomain domain)
  {
    const blender::fn::CPPType &cpp_type = blender::fn::CPPType::get<T>();
    const CustomDataType data_type = blender::bke::cpp_type_to_custom_data_type(cpp_type);
    return this->attribute_try_get_for_output_only(attribute_name, domain, data_type);
  }

 private:
  virtual const blender::bke::ComponentAttributeProviders *get_attribute_providers() const;
};

template<typename T>
inline constexpr bool is_geometry_component_v = std::is_base_of_v<GeometryComponent, T>;

/**
 * A geometry set contains zero or more geometry components. There is at most one component of each
 * type. Individual components might be shared between multiple geometries. Shared components are
 * copied automatically when write access is requested.
 *
 * Copying a geometry set is a relatively cheap operation, because it does not copy the referenced
 * geometry components.
 */
struct GeometrySet {
 private:
  using GeometryComponentPtr = blender::UserCounter<class GeometryComponent>;
  blender::Map<GeometryComponentType, GeometryComponentPtr> components_;

 public:
  GeometryComponent &get_component_for_write(GeometryComponentType component_type);
  template<typename Component> Component &get_component_for_write()
  {
    BLI_STATIC_ASSERT(is_geometry_component_v<Component>, "");
    return static_cast<Component &>(this->get_component_for_write(Component::static_type));
  }

  const GeometryComponent *get_component_for_read(GeometryComponentType component_type) const;
  template<typename Component> const Component *get_component_for_read() const
  {
    BLI_STATIC_ASSERT(is_geometry_component_v<Component>, "");
    return static_cast<const Component *>(get_component_for_read(Component::static_type));
  }

  bool has(const GeometryComponentType component_type) const;
  template<typename Component> bool has() const
  {
    BLI_STATIC_ASSERT(is_geometry_component_v<Component>, "");
    return this->has(Component::static_type);
  }

  void remove(const GeometryComponentType component_type);
  template<typename Component> void remove()
  {
    BLI_STATIC_ASSERT(is_geometry_component_v<Component>, "");
    return this->remove(Component::static_type);
  }

  void add(const GeometryComponent &component);

  blender::Vector<const GeometryComponent *> get_components_for_read() const;

  void compute_boundbox_without_instances(blender::float3 *r_min, blender::float3 *r_max) const;

  friend std::ostream &operator<<(std::ostream &stream, const GeometrySet &geometry_set);
  friend bool operator==(const GeometrySet &a, const GeometrySet &b);
  uint64_t hash() const;

  void clear();

  void ensure_owns_direct_data();

  /* Utility methods for creation. */
  static GeometrySet create_with_mesh(
      Mesh *mesh, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  static GeometrySet create_with_pointcloud(
      PointCloud *pointcloud, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);

  /* Utility methods for access. */
  bool has_mesh() const;
  bool has_pointcloud() const;
  bool has_instances() const;
  bool has_volume() const;
  const Mesh *get_mesh_for_read() const;
  const PointCloud *get_pointcloud_for_read() const;
  const Volume *get_volume_for_read() const;
  Mesh *get_mesh_for_write();
  PointCloud *get_pointcloud_for_write();
  Volume *get_volume_for_write();

  /* Utility methods for replacement. */
  void replace_mesh(Mesh *mesh, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  void replace_pointcloud(PointCloud *pointcloud,
                          GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
};

/** A geometry component that can store a mesh. */
class MeshComponent : public GeometryComponent {
 private:
  Mesh *mesh_ = nullptr;
  GeometryOwnershipType ownership_ = GeometryOwnershipType::Owned;
  /* Due to historical design choices, vertex group data is stored in the mesh, but the vertex
   * group names are stored on an object. Since we don't have an object here, we copy over the
   * names into this map. */
  blender::Map<std::string, int> vertex_group_names_;

 public:
  MeshComponent();
  ~MeshComponent();
  GeometryComponent *copy() const override;

  void clear();
  bool has_mesh() const;
  void replace(Mesh *mesh, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  void replace_mesh_but_keep_vertex_group_names(
      Mesh *mesh, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  Mesh *release();

  void copy_vertex_group_names_from_object(const struct Object &object);
  const blender::Map<std::string, int> &vertex_group_names() const;
  blender::Map<std::string, int> &vertex_group_names();

  const Mesh *get_for_read() const;
  Mesh *get_for_write();

  int attribute_domain_size(const AttributeDomain domain) const final;
  std::unique_ptr<blender::fn::GVArray> attribute_try_adapt_domain(
      std::unique_ptr<blender::fn::GVArray> varray,
      const AttributeDomain from_domain,
      const AttributeDomain to_domain) const final;

  bool is_empty() const final;

  bool owns_direct_data() const override;
  void ensure_owns_direct_data() override;

  static constexpr inline GeometryComponentType static_type = GEO_COMPONENT_TYPE_MESH;

 private:
  const blender::bke::ComponentAttributeProviders *get_attribute_providers() const final;
};

/** A geometry component that stores a point cloud. */
class PointCloudComponent : public GeometryComponent {
 private:
  PointCloud *pointcloud_ = nullptr;
  GeometryOwnershipType ownership_ = GeometryOwnershipType::Owned;

 public:
  PointCloudComponent();
  ~PointCloudComponent();
  GeometryComponent *copy() const override;

  void clear();
  bool has_pointcloud() const;
  void replace(PointCloud *pointcloud,
               GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  PointCloud *release();

  const PointCloud *get_for_read() const;
  PointCloud *get_for_write();

  int attribute_domain_size(const AttributeDomain domain) const final;

  bool is_empty() const final;

  bool owns_direct_data() const override;
  void ensure_owns_direct_data() override;

  static constexpr inline GeometryComponentType static_type = GEO_COMPONENT_TYPE_POINT_CLOUD;

 private:
  const blender::bke::ComponentAttributeProviders *get_attribute_providers() const final;
};

/** A geometry component that stores instances. */
class InstancesComponent : public GeometryComponent {
 private:
  blender::Vector<blender::float4x4> transforms_;
  blender::Vector<int> ids_;
  blender::Vector<InstancedData> instanced_data_;

  /* These almost unique ids are generated based on `ids_`, which might not contain unique ids at
   * all. They are *almost* unique, because under certain very unlikely circumstances, they are not
   * unique. Code using these ids should not crash when they are not unique but can generally
   * expect them to be unique. */
  mutable std::mutex almost_unique_ids_mutex_;
  mutable blender::Array<int> almost_unique_ids_;

 public:
  InstancesComponent();
  ~InstancesComponent() = default;
  GeometryComponent *copy() const override;

  void clear();
  void add_instance(Object *object, blender::float4x4 transform, const int id = -1);
  void add_instance(Collection *collection, blender::float4x4 transform, const int id = -1);
  void add_instance(InstancedData data, blender::float4x4 transform, const int id = -1);

  blender::Span<InstancedData> instanced_data() const;
  blender::Span<blender::float4x4> transforms() const;
  blender::Span<int> ids() const;
  blender::MutableSpan<blender::float4x4> transforms();
  int instances_amount() const;

  blender::Span<int> almost_unique_ids() const;

  bool is_empty() const final;

  bool owns_direct_data() const override;
  void ensure_owns_direct_data() override;

  static constexpr inline GeometryComponentType static_type = GEO_COMPONENT_TYPE_INSTANCES;
};

/** A geometry component that stores volume grids. */
class VolumeComponent : public GeometryComponent {
 private:
  Volume *volume_ = nullptr;
  GeometryOwnershipType ownership_ = GeometryOwnershipType::Owned;

 public:
  VolumeComponent();
  ~VolumeComponent();
  GeometryComponent *copy() const override;

  void clear();
  bool has_volume() const;
  void replace(Volume *volume, GeometryOwnershipType ownership = GeometryOwnershipType::Owned);
  Volume *release();

  const Volume *get_for_read() const;
  Volume *get_for_write();

  bool owns_direct_data() const override;
  void ensure_owns_direct_data() override;

  static constexpr inline GeometryComponentType static_type = GEO_COMPONENT_TYPE_VOLUME;
};

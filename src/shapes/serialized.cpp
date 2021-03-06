#include <mitsuba/render/mesh.h>
#include <mitsuba/render/emitter.h>
#include <mitsuba/render/sensor.h>
#include <mitsuba/core/fstream.h>
#include <mitsuba/core/zstream.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/timer.h>

NAMESPACE_BEGIN(mitsuba)

/**!

.. _shape-serialized:

Serialized mesh loader (:monosp:`serialized`)
---------------------------------------------

.. pluginparameters::

 * - filename
   - |string|
   - Filename of the OBJ file that should be loaded
 * - shape_index
   - |int|
   - A :monosp:`.serialized` file may contain several separate meshes. This parameter
     specifies which one should be loaded. (Default: 0, i.e. the first one)
 * - face_normals
   - |bool|
   - When set to |true|, any existing or computed vertex normals are
     discarded and \emph{face normals} will instead be used during rendering.
     This gives the rendered object a faceted appearance.(Default: |false|)
 * - to_world
   - |transform|
   - Specifies an optional linear object-to-world transformation.
     (Default: none, i.e. object space = world space)

The serialized mesh format represents the most space and time-efficient way
of getting geometry information into Mitsuba 2. It stores indexed triangle meshes
in a lossless gzip-based encoding that (after decompression) nicely matches up
with the internally used data structures. Loading such files is considerably
faster than the :ref:`ply <shape-ply>` plugin and orders of magnitude faster than
the :ref:`obj <shape-obj>` plugin.

Format description
******************

The :monosp:`serialized` file format uses the little endian encoding, hence
all fields below should be interpreted accordingly. The contents are structured as
follows:

.. figtable::
    :label: table-serialized-format

    .. list-table::
        :widths: 20 80
        :header-rows: 1

        * - Type
          - Content
        * - :monosp:`uint16`
          - File format identifier: :code:`0x041C`
        * - :monosp:`uint16`
          - File version identifier. Currently set to :code:`0x0004`
        * - :math:`\rightarrow`
          - From this point on, the stream is compressed by the :monosp:`DEFLATE` algorithm.
        * - :math:`\rightarrow`
          - The used encoding is that of the :monosp:`zlib` library.
        * - :monosp:`uint32`
          - An 32-bit integer whose bits can be used to specify the following flags:

            - :code:`0x0001`: The mesh data includes per-vertex normals
            - :code:`0x0002`: The mesh data includes texture coordinates
            - :code:`0x0008`: The mesh data includes vertex colors
            - :code:`0x0010`: Use face normals instead of smothly interpolated vertex normals.
              Equivalent to specifying :monosp:`face_normals=true` to the plugin.
            - :code:`0x1000`: The subsequent content is represented in single precision
            - :code:`0x2000`: The subsequent content is represented in double precision
        * - :monosp:`string`
          - A null-terminated string (utf-8), which denotes the name of the shape.
        * - :monosp:`uint64`
          - Number of vertices in the mesh
        * - :monosp:`uint64`
          - Number of triangles in the mesh
        * - :monosp:`array`
          - Array of all vertex positions (X, Y, Z, X, Y, Z, ...) specified in binary single or
            double precision format (as denoted by the flags)
        * - :monosp:`array`
          - Array of all vertex normal directions (X, Y, Z, X, Y, Z, ...) specified in binary single
            or double precision format. When the mesh has no vertex normals, this field is omitted.
        * - :monosp:`array`
          - Array of all vertex texture coordinates (U, V, U, V, ...) specified in binary single or
            double precision format. When the mesh has no texture coordinates, this field is omitted.
        * - :monosp:`array`
          - Array of all vertex colors (R, G, B, R, G, B, ...) specified in binary single or double
            precision format. When the mesh has no vertex colors, this field is omitted.
        * - :monosp:`array`
          - Indexed triangle data (:code:`[i1, i2, i3]`, :code:`[i1, i2, i3]`, ..) specified in
            :monosp:`uint32` or in :monosp:`uint64` format (the latter is used when the number of
            vertices exceeds :code:`0xFFFFFFFF`).

Multiple shapes
***************

It is possible to store multiple meshes in a single :monosp:`.serialized`
file. This is done by simply concatenating their data streams,
where every one is structured according to the above description.
Hence, after each mesh, the stream briefly reverts back to an
uncompressed format, followed by an uncompressed header, and so on.
This is neccessary for efficient read access to arbitrary sub-meshes.

End-of-file dictionary
**********************
In addition to the previous table, a :monosp:`.serialized` file also concludes with a brief summary
at the end of the file, which specifies the starting position of each sub-mesh:

.. figtable::
    :label: table-serialized-end-of-file

    .. list-table::
        :widths: 20 80
        :header-rows: 1

        * - Type
          - Content
        * - :monosp:`uint64`
          - File offset of the first mesh (in bytes)---this is always zero.
        * - :monosp:`uint64`
          - File offset of the second mesh
        * - :math:`\cdots`
          - :math:`\cdots`
        * - :monosp:`uint64`
          - File offset of the last sub-shape
        * - :monosp:`uint32`
          - Total number of meshes in the :monosp:`.serialized` file

 */

#define MTS_FILEFORMAT_HEADER     0x041C
#define MTS_FILEFORMAT_VERSION_V3 0x0003
#define MTS_FILEFORMAT_VERSION_V4 0x0004

template <typename Float, typename Spectrum>
class SerializedMesh final : public Mesh<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(Mesh, m_vertices, m_faces, m_normal_offset, m_vertex_size, m_face_size,
                    m_texcoord_offset, m_color_offset, m_name, m_bbox, m_to_world, m_vertex_count,
                    m_face_count, m_vertex_struct, m_face_struct, m_disable_vertex_normals,
                    recompute_vertex_normals, is_emitter, emitter, is_sensor, sensor, 
                    vertex, has_vertex_normals, has_vertex_texcoords, vertex_texcoord, 
                    vertex_normal, vertex_position)
    MTS_IMPORT_TYPES()

    using typename Base::ScalarSize;
    using typename Base::ScalarIndex;
    using typename Base::VertexHolder;
    using typename Base::FaceHolder;

    enum class TriMeshFlags {
        HasNormals      = 0x0001,
        HasTexcoords    = 0x0002,
        HasTangents     = 0x0004, // unused
        HasColors       = 0x0008,
        FaceNormals     = 0x0010,
        SinglePrecision = 0x1000,
        DoublePrecision = 0x2000
    };

    constexpr bool has_flag(TriMeshFlags flags, TriMeshFlags f) {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(f)) != 0;
    }
    constexpr bool has_flag(uint32_t flags, TriMeshFlags f) {
        return (flags & static_cast<uint32_t>(f)) != 0;
    }

    SerializedMesh(const Properties &props) : Base(props) {
        auto fail = [&](const std::string &descr) {
            Throw("Error while loading serialized file \"%s\": %s!", m_name, descr);
        };

        auto fs = Thread::thread()->file_resolver();
        fs::path file_path = fs->resolve(props.string("filename"));
        m_name = file_path.filename().string();

        Log(Debug, "Loading mesh from \"%s\" ..", m_name);
        if (!fs::exists(file_path))
            fail("file not found");

        // Object-space to world-space transformation
        ScalarTransform4f to_world = props.transform("to_world", ScalarTransform4f());

        /// When the file contains multiple meshes, this index specifies which one to load
        int shape_index = props.int_("shape_index", 0);
        if (shape_index < 0)
            fail("shape index must be nonnegative!");

        m_name = tfm::format("%s@%i", file_path.filename(), shape_index);

        ref<Stream> stream = new FileStream(file_path);
        Timer timer;
        stream->set_byte_order(Stream::ELittleEndian);

        short format = 0, version = 0;
        stream->read(format);
        stream->read(version);

        if (format != MTS_FILEFORMAT_HEADER)
            fail("encountered an invalid file format!");

        if (version != MTS_FILEFORMAT_VERSION_V3 &&
            version != MTS_FILEFORMAT_VERSION_V4)
            fail("encountered an incompatible file version!");

        if (shape_index != 0) {
            size_t file_size = stream->size();

            /* Determine the position of the requested substream. This
               is stored at the end of the file */
            stream->seek(file_size - sizeof(uint32_t));

            uint32_t count = 0;
            stream->read(count);

            if (shape_index > (int) count)
                fail(tfm::format("Unable to unserialize mesh, shape index is "
                                 "out of range! (requested %i out of 0..%i)",
                                 shape_index, count - 1));

            // Seek to the correct position
            if (version == MTS_FILEFORMAT_VERSION_V4) {
                stream->seek(file_size -
                             sizeof(uint64_t) * (count - shape_index) -
                             sizeof(uint32_t));
                size_t offset = 0;
                stream->read(offset);
                stream->seek(offset);
            } else {
                Assert(version == MTS_FILEFORMAT_VERSION_V3);
                stream->seek(file_size -
                             sizeof(uint32_t) * (count - shape_index + 1));
                uint32_t offset = 0;
                stream->read(offset);
                stream->seek(offset);
            }
            stream->skip(sizeof(short) * 2); // Skip the header
        }

        stream = new ZStream(stream);
        stream->set_byte_order(Stream::ELittleEndian);

        uint32_t flags = 0;
        stream->read(flags);
        if (version == MTS_FILEFORMAT_VERSION_V4) {
            char ch = 0;
            m_name = "";
            do {
                stream->read(ch);
                if (ch == 0)
                    break;
                m_name += ch;
            } while (true);
        }

        size_t vertex_count, face_count;
        stream->read(vertex_count);
        stream->read(face_count);

        m_vertex_struct = new Struct();
        for (auto name : { "x", "y", "z" })
            m_vertex_struct->append(name, struct_type_v<ScalarFloat>);

        if (!m_disable_vertex_normals) {
            for (auto name : { "nx", "ny", "nz" })
                m_vertex_struct->append(name, struct_type_v<ScalarFloat>);
            m_normal_offset = (ScalarIndex) m_vertex_struct->offset("nx");
        }

        if (has_flag(flags, TriMeshFlags::HasTexcoords)) {
            for (auto name : { "u", "v" })
                m_vertex_struct->append(name, struct_type_v<ScalarFloat>);
            m_texcoord_offset = (ScalarIndex) m_vertex_struct->offset("u");
        }

        if (has_flag(flags, TriMeshFlags::HasColors)) {
            for (auto name : { "r", "g", "b" })
                m_vertex_struct->append(name, struct_type_v<ScalarFloat>);
            m_color_offset = (ScalarIndex) m_vertex_struct->offset("r");
        }

        m_face_struct = new Struct();
        for (size_t i = 0; i < 3; ++i)
            m_face_struct->append(tfm::format("i%i", i), struct_type_v<ScalarIndex>);

        m_vertex_size = (ScalarSize) m_vertex_struct->size();
        m_vertex_count = (ScalarSize) vertex_count;
        m_vertices = VertexHolder(new uint8_t[(m_vertex_count + 1) * m_vertex_size]);

        m_face_size = (ScalarSize) m_face_struct->size();
        m_face_count = (ScalarSize) face_count;
        m_faces = FaceHolder(new uint8_t[(m_face_count + 1) * m_face_size]);

        bool double_precision = has_flag(flags, TriMeshFlags::DoublePrecision);
        read_helper(stream, double_precision, m_vertex_struct->offset("x"), 3);

        if (has_flag(flags, TriMeshFlags::HasNormals)) {
            if (m_disable_vertex_normals)
                // Skip over vertex normals provided in the file.
                advance_helper(stream, double_precision, 3);
            else
                read_helper(stream, double_precision,
                            m_vertex_struct->offset("nx"), 3);
        }

        if (has_flag(flags, TriMeshFlags::HasTexcoords))
            read_helper(stream, double_precision, m_vertex_struct->offset("u"), 2);

        if (has_flag(flags, TriMeshFlags::HasColors))
            read_helper(stream, double_precision, m_vertex_struct->offset("r"), 3);

        stream->read(m_faces.get(), m_face_count * sizeof(ScalarIndex) * 3);

        Log(Debug, "\"%s\": read %i faces, %i vertices (%s in %s)",
            m_name, m_face_count, m_vertex_count,
            util::mem_string(m_face_count * m_face_struct->size() +
                             m_vertex_count * m_vertex_struct->size()),
            util::time_string(timer.value())
        );

        // Post-processing
        for (ScalarSize i = 0; i < m_vertex_count; ++i) {
            ScalarPoint3f p = to_world * vertex_position(i);
            store_unaligned(vertex(i), p);
            m_bbox.expand(p);

            if (has_vertex_normals()) {
                ScalarNormal3f n = normalize(to_world * vertex_normal(i));
                store_unaligned(vertex(i) + m_normal_offset, n);
            }

            if (has_vertex_texcoords()) {
                ScalarPoint2f uv = vertex_texcoord(i);
                store_unaligned(vertex(i) + m_texcoord_offset, uv);
            }
        }

        if (!m_disable_vertex_normals && !has_flag(flags, TriMeshFlags::HasNormals))
            recompute_vertex_normals();

        if (is_emitter())
            emitter()->set_shape(this);
        if (is_sensor())
            sensor()->set_shape(this);
    }

    void read_helper(Stream *stream, bool dp, size_t offset, size_t dim) {
        if (dp) {
            std::unique_ptr<double[]> values(new double[m_vertex_count * dim]);
            stream->read_array(values.get(), m_vertex_count * dim);

            if constexpr (std::is_same_v<ScalarFloat, double>) {
                for (size_t i = 0; i < m_vertex_count; ++i) {
                    const double *src = values.get() + dim * i;
                    double *dst = (double *) (vertex(i) + offset);
                    memcpy(dst, src, sizeof(double) * dim);
                }
            } else {
                for (size_t i = 0; i < m_vertex_count; ++i) {
                    const double *src = values.get() + dim * i;
                    float *dst = (float *) (vertex(i) + offset);
                    for (size_t d = 0; d < dim; ++d)
                        dst[d] = (float) src[d];
                }
            }
        } else {
            std::unique_ptr<float[]> values(new float[m_vertex_count * dim]);
            stream->read_array(values.get(), m_vertex_count * dim);

            if constexpr (std::is_same_v<ScalarFloat, float>) {
                for (size_t i = 0; i < m_vertex_count; ++i) {
                    const float *src = values.get() + dim * i;
                    void *dst = vertex(i) + offset;
                    memcpy(dst, src, sizeof(float) * dim);
                }
            } else {
                for (size_t i = 0; i < m_vertex_count; ++i) {
                    const float *src = values.get() + dim * i;
                    double *dst = (double *) (vertex(i) + offset);
                    for (size_t d = 0; d < dim; ++d)
                        dst[d] = (double) src[d];
                }
            }
        }
    }

    /**
     * Simply advances the stream without outputing to the mesh.
     * Since compressed streams do not provide `tell` and `seek`
     * implementations, we have to read and discard the data.
     */
    void advance_helper(Stream *stream, bool dp, size_t dim) {
        if (dp) {
            std::unique_ptr<double[]> values(new double[m_vertex_count * dim]);
            stream->read_array(values.get(), m_vertex_count * dim);
        } else {
            std::unique_ptr<float[]> values(new float[m_vertex_count * dim]);
            stream->read_array(values.get(), m_vertex_count * dim);
        }
    }

    MTS_DECLARE_CLASS()
};

MTS_IMPLEMENT_CLASS_VARIANT(SerializedMesh, Mesh)
MTS_EXPORT_PLUGIN(SerializedMesh, "Serialized mesh file")
NAMESPACE_END(mitsuba)

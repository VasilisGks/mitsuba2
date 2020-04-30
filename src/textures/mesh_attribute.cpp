#include <mitsuba/render/texture.h>
#include <mitsuba/render/interaction.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/transform.h>

NAMESPACE_BEGIN(mitsuba)

/**!
TODO: add description
 */

template <typename Float, typename Spectrum>
class MeshAttribute final : public Texture<Float, Spectrum> {
public:
    MTS_IMPORT_TYPES(Texture)

    MeshAttribute(const Properties &props)
    : Texture(props) {
        m_name = props.string("name");
        if (m_name.find("vertex_") == std::string::npos && m_name.find("face_") == std::string::npos)
            Throw("Invalid mesh attribute name: must be start with either \"vertex_\" or \"face_\" but was \"%s\".", m_name.c_str());

        m_scale = props.float_("scale", 1.f);
    }

    const std::string& name() const { return m_name; }

    UnpolarizedSpectrum eval(const SurfaceInteraction3f &si, Mask active) const override {
        MTS_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);
        auto target = select(neq(si.instance, nullptr), si.instance, si.shape);
        return target->eval_attribute(m_name, si, active) * m_scale;
    }

    Float eval_1(const SurfaceInteraction3f &si, Mask active = true) const override {
        MTS_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);
        auto target = select(neq(si.instance, nullptr), si.instance, si.shape);
        return target->eval_attribute_1(m_name, si, active) * m_scale;
    }

    Color3f eval_3(const SurfaceInteraction3f &si, Mask active = true) const override {
        MTS_MASKED_FUNCTION(ProfilerPhase::TextureEvaluate, active);
        auto target = select(neq(si.instance, nullptr), si.instance, si.shape);
        return target->eval_attribute_3(m_name, si, active) * m_scale;
    }

    void traverse(TraversalCallback *callback) override {
        callback->put_parameter("scale", m_scale);
    }

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "MeshAttribute[" << std::endl
            << "  name = \"" << m_name << "\"," << std::endl
            << "  scale = \"" << m_scale << "\"" << std::endl
            << "]";
        return oss.str();
    }

    MTS_DECLARE_CLASS()
protected:
    std::string m_name;
    float m_scale;
};

MTS_IMPLEMENT_CLASS_VARIANT(MeshAttribute, Texture)
MTS_EXPORT_PLUGIN(MeshAttribute, "Mesh attribute")

NAMESPACE_END(mitsuba)

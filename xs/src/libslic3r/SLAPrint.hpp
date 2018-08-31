#ifndef slic3r_SLAPrint_hpp_
#define slic3r_SLAPrint_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "ExPolygonCollection.hpp"
#include "Fill/Fill.hpp"
#include "Model.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "SVG.hpp"

namespace Slic3r {

class SLAPrint
{
    public:
    SLAPrintConfig config;
    
    class Layer {
        public:
        ExPolygonCollection slices;
        ExPolygonCollection perimeters;
        ExtrusionEntityCollection infill;
        ExPolygonCollection solid_infill;
        float slice_z, print_z;
        bool solid;
        
        Layer(float _slice_z, float _print_z)
            : slice_z(_slice_z), print_z(_print_z), solid(true) {};
    };
    std::vector<Layer> layers;
    
    class SupportPillar : public Point {
        public:
        size_t top_layer, bottom_layer;
        SupportPillar(const Point &p) : Point(p), top_layer(0), bottom_layer(0) {};
    };
    std::vector<SupportPillar> sm_pillars;

    SLAPrint(Model* _model) : model(_model) {};
    
    SLAPrint(Model* _model, FILE *_f) : model(_model), layer_nr(0) {
        f = _f;
    };
    bool slice();
    void write_svg(const std::string &outputfile) const;
    bool write_svg_layer(const size_t k);
    bool write_svg_header() const;
    bool write_svg_footer() const;
    size_t get_layers_size();
    
    private:
    Model* model;
    BoundingBoxf3 bb;
    FILE *f;
    size_t layer_nr; // to keep track of layer nr in write_svg_layer
    
    void _infill_layer(size_t i, const Fill* fill);
    coordf_t sm_pillars_radius() const;
    std::string _SVG_path_d(const Polygon &polygon) const;
    std::string _SVG_path_d(const ExPolygon &expolygon) const;
    std::string get_time() const;
    bool update_layer_nr();
};

}

#endif

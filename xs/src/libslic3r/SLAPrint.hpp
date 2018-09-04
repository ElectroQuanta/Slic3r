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

    SLAPrint(Model* _model) : model(_model), id(getCount()){};
    
    SLAPrint(Model* _model, FILE *_f) : model(_model), layer_nr(0), id(getCount()) {
        f = _f;
    };
    SLAPrint(Model* _model, const std::string _fname, const std::string _infillclr) :
        model(_model), fname(_fname), infill_clr(_infillclr), layer_nr(0), id(getCount()) {};

    SLAPrint(Model* _model, const std::string _fname) :
        model(_model), fname(_fname), layer_nr(0), id( getCount() )
    {
        count++; // incrementing count
    };
    
    void slice();
    void write_svg(const std::string &outputfile) const;
    bool write_svg_layer(const size_t k);
    bool write_svg_header() const;
    bool write_svg_footer() const;
    size_t get_layers_size();
    
    private:
    Model* model;
    BoundingBoxf3 bb;
    FILE *f;
    const std::string fname; 
    const std::string infill_clr; 
    size_t layer_nr; // to keep track of layer nr in write_svg_layer
    const size_t id; // based on the nr of objects created 

    static size_t count; // # of objects instantiated
    static const std::vector<std::string> fill_clrs;
    
    void _infill_layer(size_t i, const Fill* fill);
    coordf_t sm_pillars_radius() const;
    std::string _SVG_path_d(const Polygon &polygon) const;
    std::string _SVG_path_d(const ExPolygon &expolygon) const;
    std::string get_time() const;
    bool is_layer_nr_valid();
    // get the current fill color based on # obj
    std::string getFillColor() const;
    // get the nr. of objects instantiated
    static size_t getCount();
    // create a polyline path instead of path_d, for compatibility issues
    std::string _SVG_polyline(const Polygon &polygon) const;
    std::string _SVG_polyline(const ExPolygon &expolygon,
                              const std::string fill_type) const;
};

}

#endif

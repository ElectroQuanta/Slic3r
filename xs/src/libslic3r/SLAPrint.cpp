#include "SLAPrint.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "Fill/Fill.hpp"
#include "Geometry.hpp"
#include "Surface.hpp"
#include <iostream>
#include <iomanip> 
#include <ctime>
#include <complex>
#include <cstdio>

namespace Slic3r {

// Statics initialization
size_t SLAPrint::count = 0;
const std::vector<std::string> SLAPrint::fill_clrs = {"white", "red", "blue", "yellow"};
size_t SLAPrint::getCount(){return count;}
float SLAPrint::size_y = 0.0; // size_y of bounding box
float SLAPrint::min_x = 0.0; // min_x of bounding box
float SLAPrint::min_y = 0.0; // min_y of bounding box

void
SLAPrint::slice()
{
    TriangleMesh mesh = this->model->mesh();
    mesh.repair();
    
    // align to origin taking raft into account
    this->bb = mesh.bounding_box();
    if (this->config.raft_layers > 0) {
        this->bb.min.x -= this->config.raft_offset.value;
        this->bb.min.y -= this->config.raft_offset.value;
        this->bb.max.x += this->config.raft_offset.value;
        this->bb.max.y += this->config.raft_offset.value;
    }
    mesh.translate(0, 0, -bb.min.z);
    this->bb.translate(0, 0, -bb.min.z);
    
    // if we are generating a raft, first_layer_height will not affect mesh slicing
    const float lh       = this->config.layer_height.value;
    const float first_lh = this->config.first_layer_height.value;
    
    // generate the list of Z coordinates for mesh slicing
    // (we slice each layer at half of its thickness)
    this->layers.clear();
    {
        const float first_slice_lh = (this->config.raft_layers > 0) ? lh : first_lh;
        this->layers.push_back(Layer(first_slice_lh/2, first_slice_lh));
    }
    while (this->layers.back().print_z + lh/2 <= mesh.stl.stats.max.z) {
        this->layers.push_back(Layer(this->layers.back().print_z + lh/2, this->layers.back().print_z + lh));
    }
    
    // perform slicing and generate layers
    {
        std::vector<float> slice_z;
        for (size_t i = 0; i < this->layers.size(); ++i)
            slice_z.push_back(this->layers[i].slice_z);
        
        std::vector<ExPolygons> slices;
        TriangleMeshSlicer<Z>(&mesh).slice(slice_z, &slices);
        
        for (size_t i = 0; i < slices.size(); ++i)
            this->layers[i].slices.expolygons = slices[i];
    }
    
    // generate infill
    if (this->config.fill_density < 100) {
        std::unique_ptr<Fill> fill(Fill::new_from_type(this->config.fill_pattern.value));
        fill->bounding_box.merge(Point::new_scale(bb.min.x, bb.min.y));
        fill->bounding_box.merge(Point::new_scale(bb.max.x, bb.max.y));
        fill->min_spacing   = this->config.get_abs_value("infill_extrusion_width", this->config.layer_height.value);
        fill->angle         = Geometry::deg2rad(this->config.fill_angle.value);
        fill->density       = this->config.fill_density.value/100;

        // Minimum spacing has a lower bound of > 0. Set to a sane default 
        // if the user gets an invalid value here.
        fill->min_spacing = (fill->min_spacing <= 0 ? 0.5 : fill->min_spacing);

        parallelize<size_t>(
            0,
            this->layers.size()-1,
            boost::bind(&SLAPrint::_infill_layer, this, _1, fill.get()),
            this->config.threads.value
        );
    }
    
    // generate support material
    this->sm_pillars.clear();
    ExPolygons overhangs;
    if (this->config.support_material) {
        // flatten and merge all the overhangs
        {
            Polygons pp;
            for (std::vector<Layer>::const_iterator it = this->layers.begin()+1; it != this->layers.end(); ++it)
                pp += diff(it->slices, (it - 1)->slices);
            overhangs = union_ex(pp);
        }
        
        // generate points following the shape of each island
        Points pillars_pos;
        const coordf_t spacing = scale_(this->config.support_material_spacing);
        const coordf_t radius  = scale_(this->sm_pillars_radius());
        for (ExPolygons::const_iterator it = overhangs.begin(); it != overhangs.end(); ++it) {
            // leave a radius/2 gap between pillars and contour to prevent lateral adhesion
            for (float inset = radius * 1.5;; inset += spacing) {
                // inset according to the configured spacing
                Polygons curr = offset(*it, -inset);
                if (curr.empty()) break;
                
                // generate points along the contours
                for (Polygons::const_iterator pg = curr.begin(); pg != curr.end(); ++pg) {
                    Points pp = pg->equally_spaced_points(spacing);
                    for (Points::const_iterator p = pp.begin(); p != pp.end(); ++p)
                        pillars_pos.push_back(*p);
                }
            }
        }
        
        // for each pillar, check which layers it applies to
        for (Points::const_iterator p = pillars_pos.begin(); p != pillars_pos.end(); ++p) {
            SupportPillar pillar(*p);
            bool object_hit = false;
            
            // check layers top-down
            for (int i = this->layers.size()-1; i >= 0; --i) {
                // check whether point is void in this layer
                if (!this->layers[i].slices.contains(*p)) {
                    // no slice contains the point, so it's in the void
                    if (pillar.top_layer > 0) {
                        // we have a pillar, so extend it
                        pillar.bottom_layer = i + this->config.raft_layers;
                    } else if (object_hit) {
                        // we don't have a pillar and we're below the object, so create one
                        pillar.top_layer = i + this->config.raft_layers;
                    }
                } else {
                    if (pillar.top_layer > 0) {
                        // we have a pillar which is not needed anymore, so store it and initialize a new potential pillar
                        this->sm_pillars.push_back(pillar);
                        pillar = SupportPillar(*p);
                    }
                    object_hit = true;
                }
            }
            if (pillar.top_layer > 0) this->sm_pillars.push_back(pillar);
        }
    }
    
    // generate a solid raft if requested
    // (do this after support material because we take support material shape into account)
    if (this->config.raft_layers > 0) {
        ExPolygons raft = this->layers.front().slices + overhangs;  // take support material into account
        raft = offset_ex(raft, scale_(this->config.raft_offset));
        for (int i = this->config.raft_layers; i >= 1; --i) {
            this->layers.insert(this->layers.begin(), Layer(0, first_lh + lh * (i-1)));
            this->layers.front().slices = raft;
        }
        
        // prepend total raft height to all sliced layers
        for (size_t i = this->config.raft_layers; i < this->layers.size(); ++i)
            this->layers[i].print_z += first_lh + lh * (this->config.raft_layers-1);
    }

    // set bounding box dims
    set_bb_dims();
}

void
SLAPrint::_infill_layer(size_t i, const Fill* _fill)
{
    Layer &layer = this->layers[i];
    
    const float shell_thickness = this->config.get_abs_value("perimeter_extrusion_width", this->config.layer_height.value);
    
    // In order to detect what regions of this layer need to be solid,
    // perform an intersection with layers within the requested shell thickness.
    Polygons internal = layer.slices;
    for (size_t j = 0; j < this->layers.size(); ++j) {
        const Layer &other = this->layers[j];
        if (std::abs(other.print_z - layer.print_z) > shell_thickness) continue;
    
        if (j == 0 || j == this->layers.size()-1) {
            internal.clear();
            break;
        } else if (i != j) {
            internal = intersection(internal, other.slices);
            if (internal.empty()) break;
        }
    }
    
    // If we have no internal infill, just print the whole layer as a solid slice.
    if (internal.empty()) return;
    layer.solid = false;
    
    const Polygons infill = offset(layer.slices, -scale_(shell_thickness));
    
    // Generate solid infill
    layer.solid_infill << diff_ex(infill, internal, true);
    
    // Generate internal infill
    {
        std::unique_ptr<Fill> fill(_fill->clone());
        fill->layer_id = i;
        fill->z        = layer.print_z;
        
        ExtrusionPath templ(erInternalInfill);

        const ExPolygons internal_ex = intersection_ex(infill, internal);
        for (ExPolygons::const_iterator it = internal_ex.begin(); it != internal_ex.end(); ++it) {
            Polylines polylines = fill->fill_surface(Surface(stInternal, *it));
            templ.width = fill->spacing(); // fill->spacing doesn't have anything defined until after fill_surface
            layer.infill.append(polylines, templ);
        }
    }
    
    // Generate perimeter(s).
    layer.perimeters << diff_ex(
        layer.slices,
        offset(layer.slices, -scale_(shell_thickness))
    );
}

void
SLAPrint::write_svg(const std::string &outputfile) const
{
    const Sizef3 size = this->bb.size();
    const double support_material_radius = sm_pillars_radius();
    
    FILE* f = fopen(outputfile.c_str(), "w");
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.0//EN\" \"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd\">\n"
        "<svg width=\"%f\" height=\"%f\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:svg=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:slic3r=\"http://slic3r.org/namespaces/slic3r\" viewport-fill=\"black\">\n"
        "<!-- Generated using Slic3r %s http://slic3r.org/ on %s -->\n"
            , size.x, size.y, SLIC3R_VERSION, get_time().c_str());
    
    for (size_t i = 0; i < this->layers.size(); ++i) {
        const Layer &layer = this->layers[i];
        fprintf(f,
            "\t<g id=\"layer%zu\" slic3r:z=\"%0.4f\" slic3r:slice-z=\"%0.4f\" slic3r:layer-height=\"%0.4f\">\n",
            i,
            layer.print_z,
            layer.slice_z,
            layer.print_z - ((i == 0) ? 0. : this->layers[i-1].print_z)
        );
        
        if (layer.solid) {
            const ExPolygons &slices = layer.slices.expolygons;
            for (ExPolygons::const_iterator it = slices.begin(); it != slices.end(); ++it) {
                std::string pd = this->_SVG_path_d(*it);
                
                fprintf(f,"\t\t<path d=\"%s\" style=\"fill: %s; stroke: %s; stroke-width: %s; fill-type: evenodd\" slic3r:area=\"%0.4f\" />\n",
                    pd.c_str(), "white", "black", "0", unscale(unscale(it->area()))
                );
            }
        } else {
            // Perimeters.
            for (ExPolygons::const_iterator it = layer.perimeters.expolygons.begin();
                it != layer.perimeters.expolygons.end(); ++it) {
                std::string pd = this->_SVG_path_d(*it);
                
                fprintf(f,"\t\t<path d=\"%s\" style=\"fill: %s; stroke: %s; stroke-width: %s; fill-type: evenodd\" slic3r:type=\"perimeter\" />\n",
                    pd.c_str(), "white", "black", "0"
                );
            }
            
            // Solid infill.
            for (ExPolygons::const_iterator it = layer.solid_infill.expolygons.begin();
                it != layer.solid_infill.expolygons.end(); ++it) {
                std::string pd = this->_SVG_path_d(*it);
                
                fprintf(f,"\t\t<path d=\"%s\" style=\"fill: %s; stroke: %s; stroke-width: %s; fill-type: evenodd\" slic3r:type=\"solid-infill\" />\n",
                    pd.c_str(), "white", "black", "0"
                );
            }
            
            // Internal infill.
            for (ExtrusionEntitiesPtr::const_iterator it = layer.infill.entities.begin();
                it != layer.infill.entities.end(); ++it) {
                const ExPolygons infill = union_ex((*it)->grow());
                
                for (ExPolygons::const_iterator e = infill.begin(); e != infill.end(); ++e) {
                    std::string pd = this->_SVG_path_d(*e);
                
                    fprintf(f,"\t\t<path d=\"%s\" style=\"fill: %s; stroke: %s; stroke-width: %s; fill-type: evenodd\" slic3r:type=\"internal-infill\" />\n",
                        pd.c_str(), "white", "black", "0"
                    );
                }
            }
        }
        
        // don't print support material in raft layers
        if (i >= (size_t)this->config.raft_layers) {
            // look for support material pillars belonging to this layer
            for (std::vector<SupportPillar>::const_iterator it = this->sm_pillars.begin(); it != this->sm_pillars.end(); ++it) {
                if (!(it->top_layer >= i && it->bottom_layer <= i)) continue;
            
                // generate a conic tip
                float radius = fminf(
                    support_material_radius,
                    (it->top_layer - i + 1) * this->config.layer_height.value
                );
            
                fprintf(f,"\t\t<circle cx=\"%f\" cy=\"%f\" r=\"%f\" stroke-width=\"0\" fill=\"white\" slic3r:type=\"support\" />\n",
                    unscale(it->x) - this->bb.min.x,
                    size.y - (unscale(it->y) - this->bb.min.y),
                    radius
                );
            }
        }
        
        fprintf(f,"\t</g>\n");
    }
    fprintf(f,"</svg>\n");
    // Ensure that the output gets written.
    fflush(f);

    // Close the file.
    fclose(f);
}

coordf_t
SLAPrint::sm_pillars_radius() const
{
    coordf_t radius = this->config.support_material_extrusion_width.get_abs_value(this->config.support_material_spacing)/2;
    if (radius == 0) radius = this->config.support_material_spacing / 3; // auto
    return radius;
}

std::string
SLAPrint::_SVG_path_d(const Polygon &polygon) const
{
    const Sizef3 size = this->bb.size();
    std::ostringstream d;
    d << "M ";
    for (Points::const_iterator p = polygon.points.begin(); p != polygon.points.end(); ++p) {
        d << unscale(p->x) - this->bb.min.x << " ";
        d << size.y - (unscale(p->y) - this->bb.min.y) << " ";  // mirror Y coordinates as SVG uses downwards Y
    }
    d << "z";
    return d.str();
}

std::string
SLAPrint::_SVG_path_d(const ExPolygon &expolygon) const
{
    std::string pd;
    const Polygons pp = expolygon;
    for (Polygons::const_iterator mp = pp.begin(); mp != pp.end(); ++mp) 
        pd += this->_SVG_path_d(*mp) + " ";
    return pd;
}

std::string
SLAPrint::get_time() const
{
   std::time_t t = std::time(nullptr);
   std::ostringstream ts;
   ts << std::put_time(std::localtime(&t), "%c %Z");
   return ts.str(); 
}

bool SLAPrint::write_svg_header() const
{
    //if( !this->f )
    //    return false;
    FILE *f = fopen(this->fname.c_str(), "a");
    if(!f)
    {
        std::cerr << "Could not open file\n"; 
        return false;
    }

    const Sizef3 size = this->bb.size();
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.0//EN\" \"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd\">\n"
        "<svg width=\"%f\" height=\"%f\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:svg=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:slic3r=\"http://slic3r.org/namespaces/slic3r\" viewport-fill=\"black\">\n"
        "<!-- Generated using Slic3r %s http://slic3r.org/ on %s -->\n"
            , size.x, size.y, SLIC3R_VERSION, get_time().c_str());

    fflush(f);
    fclose(f);

    return true;
}

bool SLAPrint::is_layer_nr_valid()
{
    return (this->layer_nr < this->layers.size());
}

size_t SLAPrint::get_layers_size()
{
    return this->layers.size();
}

/// \brief Prints layers consecutively
/// \param k: prints to file the correct layer id nr tag
bool SLAPrint::write_svg_layer(const size_t k)
{
    FILE *f = fopen(this->fname.c_str(), "a");
    if(!f)
    {
        std::cerr << "Could not open file\n"; 
        return false;
    }

    // check layer_nr
    if( !this->is_layer_nr_valid() )
        return false;

    std::cout << "Layer Nr: " << this->layer_nr
              << "\tK: " << k << std::endl;
    
    const Sizef3 size = this->bb.size();
    const double support_material_radius = sm_pillars_radius();
    size_t i = this->layer_nr;
    const Layer &layer = this->layers[i];
    fprintf(f,
            "\t<g id=\"lay%zu_mat%zu\" slic3r:z=\"%0.4f\" slic3r:slice-z=\"%0.4f\" slic3r:layer-height=\"%0.4f\" slic3r:mat=\"%zu\">\n",
        k,
        this->id + 1,    
        layer.print_z,
        layer.slice_z,
        layer.print_z - ((i == 0) ? 0. : this->layers[i-1].print_z),
        this->id + 1
    );

    if (layer.solid) {
        const ExPolygons &slices = layer.slices.expolygons;
        for (ExPolygons::const_iterator it = slices.begin(); it != slices.end(); ++it) {
            std::string pd = this->_SVG_polyline(*it, "");
            fprintf(f,"%s", pd.c_str() );
        }
    } else {
        // Perimeters.
        for (ExPolygons::const_iterator it = layer.perimeters.expolygons.begin();
            it != layer.perimeters.expolygons.end(); ++it) {
            const std::string fill_type = "perimeter";
            std::string pd = this->_SVG_polyline(*it, fill_type);
            fprintf(f,"%s", pd.c_str() );
        }

        // Solid infill.
        for (ExPolygons::const_iterator it = layer.solid_infill.expolygons.begin();
            it != layer.solid_infill.expolygons.end(); ++it) {
            const std::string fill_type = "solid-infill";
            std::string pd = this->_SVG_polyline(*it, fill_type);
            fprintf(f,"%s", pd.c_str() );
        }

        // Internal infill.
        for (ExtrusionEntitiesPtr::const_iterator it = layer.infill.entities.begin();
            it != layer.infill.entities.end(); ++it) {
            const ExPolygons infill = union_ex((*it)->grow());

            for (ExPolygons::const_iterator e = infill.begin(); e != infill.end(); ++e) {
                const std::string fill_type = "internal-infill";
                std::string pd = this->_SVG_polyline(*e, fill_type);
                fprintf(f,"%s", pd.c_str() );
            }
        }
    }

    // don't print support material in raft layers
    if (i >= (size_t)this->config.raft_layers) {
        // look for support material pillars belonging to this layer
        for (std::vector<SupportPillar>::const_iterator it = this->sm_pillars.begin(); it != this->sm_pillars.end(); ++it) {
            if (!(it->top_layer >= i && it->bottom_layer <= i)) continue;

            // generate a conic tip
            float radius = fminf(
                support_material_radius,
                (it->top_layer - i + 1) * this->config.layer_height.value
            );

            fprintf(f,"\t\t<circle cx=\"%f\" cy=\"%f\" r=\"%f\" stroke-width=\"0\" fill=\"white\" slic3r:type=\"support\" />\n",
                unscale(it->x) - this->bb.min.x,
                size.y - (unscale(it->y) - this->bb.min.y),
                radius
            );
        }
    }

    fprintf(f,"\t</g>\n");

// Update layer nr sentinel value
this->layer_nr++;
    
// Ensure that the output gets written.
fflush(f);
fclose(f);

return true; 
        
}

bool SLAPrint::write_svg_footer() const
{
    FILE *f = fopen(this->fname.c_str(), "a");
    if(!f)
    {
        std::cerr << "Could not open file\n"; 
        return false;
    }

    fprintf(f,"</svg>\n");
    
    fflush(f);
    fclose(f);

    return true;
}

std::string SLAPrint::getFillColor() const
{
    return fill_clrs[ this->id % fill_clrs.size() ];
}

std::string SLAPrint::_SVG_polyline(const Polygon &polygon) const
{
    //const Sizef3 size = this->bb.size();
    std::ostringstream d;

    // get first point to close path
    Points::const_iterator p = polygon.points.begin();
    float x_init = unscale(p->x) - min_x;
    float y_init = size_y - (unscale(p->y) - min_y);

    // Obtain path
    for ( ; p != polygon.points.end(); ++p) {
        d << unscale(p->x) - min_x << ",";
        d << size_y - (unscale(p->y) - min_y) << " ";  // mirror Y coordinates as SVG uses downwards Y
    }

    // Repeat 1st point to close path
    d << x_init << ",";
    d << y_init;

    return d.str();
}
std::string SLAPrint::_SVG_polyline(const ExPolygon &expolygon,
                                    const std::string fill_type) const
{
    std::string pd;
    const Polygons pp = expolygon;
    std::string fill_clr = "none";
    std::string stroke_clr = getFillColor();
    std::string pol_tag = "<polyline points= ";
    float stroke_width = 0.1;
    std::ostringstream d;
    for (Polygons::const_iterator mp = pp.begin(); mp != pp.end(); ++mp)
    {
        pd = this->_SVG_polyline(*mp) + " ";
        d << "\t\t" << pol_tag << "\"" << pd << "\"";
        d << " style=\"fill: " << fill_clr
          << "; stroke: " << stroke_clr
          << "; stroke-width: " << stroke_width
          << "; fill-type: evenodd\" slic3r:type="
          << "\"" << fill_type << "\" />\n";
    }
    return d.str();
}

void SLAPrint::set_bb_dims()
{
    const Sizef3 size = this->bb.size();
    if( getCount() == 1 ) // pass the first size to dimension statics
    {
        min_x = this->bb.min.x;
        min_y = this->bb.min.y;
        size_y = size.y;
    }
    else
    {
        if(min_x > this->bb.min.x)
            min_x = this->bb.min.x;
        if(min_y > this->bb.min.y)
            min_y = this->bb.min.y;
        if(size_y < size.y)
            size_y = size.y;
    }
}
    

}


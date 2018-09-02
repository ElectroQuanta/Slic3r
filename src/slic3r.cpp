#include "ConfigBase.hpp"
#include "Geometry.hpp"
#include "IO.hpp"
#include "Model.hpp"
#include "SLAPrint.hpp"
#include "Print.hpp"
#include "TriangleMesh.hpp"
#include "libslic3r.h"
#include <cstdio>
#include <string>
#include <cstring>
#include <iostream>
#include <math.h>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/iostream.hpp>


#ifdef USE_WX
    #include "GUI/GUI.hpp"
#endif

/// utility function for displaying CLI usage
void printUsage();

// fill colors
std::vector<std::string> fill_clrs = {"white", "red", "blue", "yellow"};
std::string getFillColor(size_t id);

using namespace Slic3r;

int
main(int argc, char **argv)
{
    // Convert arguments to UTF-8 (needed on Windows).
    // argv then points to memory owned by a.
    boost::nowide::args a(argc, argv);
    
    // parse all command line options into a DynamicConfig
    ConfigDef config_def;
    config_def.merge(cli_config_def);
    config_def.merge(print_config_def);
    DynamicConfig config(&config_def);
    t_config_option_keys input_files;
    // if any option is unsupported, print usage and abort immediately
    if ( !config.read_cli(argc, argv, &input_files) )
    {
        printUsage();
        return 0;
    }
    
    // apply command line options to a more handy CLIConfig
    CLIConfig cli_config;
    cli_config.apply(config, true);
    
    DynamicPrintConfig print_config;

#ifdef USE_WX
    if (cli_config.gui) {
        GUI::App *gui = new GUI::App();

        GUI::App::SetInstance(gui);
        wxEntry(argc, argv);
    }
#else
    if (cli_config.gui) {
        std::cout << "GUI support has not been built." << "\n";
    }
#endif    
    // load config files supplied via --load
    for (const std::string &file : cli_config.load.values) {
        if (!boost::filesystem::exists(file)) {
            boost::nowide::cout << "No such file: " << file << std::endl;
            exit(1);
        }
        
        DynamicPrintConfig c;
        try {
            c.load(file);
        } catch (std::exception &e) {
            boost::nowide::cout << "Error while reading config file: " << e.what() << std::endl;
            exit(1);
        }
        c.normalize();
        print_config.apply(c);
    }
    
    // apply command line options to a more specific DynamicPrintConfig which provides normalize()
    // (command line options override --load files)
    print_config.apply(config, true);
    print_config.normalize();
    
    // write config if requested
    if (!cli_config.save.value.empty()) print_config.save(cli_config.save.value);
    
    // read input file(s) if any
    std::vector<Model> models;
    for (const t_config_option_key &file : input_files) {
        if (!boost::filesystem::exists(file)) {
            boost::nowide::cerr << "No such file: " << file << std::endl;
            exit(1);
        }
        
        Model model;
        try {
            model = Model::read_from_file(file);
        } catch (std::exception &e) {
            boost::nowide::cerr << file << ": " << e.what() << std::endl;
            exit(1);
        }
        
        if (model.objects.empty()) {
            boost::nowide::cerr << "Error: file is empty: " << file << std::endl;
            continue;
        }
        
        model.add_default_instances();
        
        // apply command line transform options
        for (ModelObject* o : model.objects) {
            if (cli_config.scale_to_fit.is_positive_volume())
                o->scale_to_fit(cli_config.scale_to_fit.value);
            
            // TODO: honor option order?
            o->scale(cli_config.scale.value);
            o->rotate(Geometry::deg2rad(cli_config.rotate_x.value), X);
            o->rotate(Geometry::deg2rad(cli_config.rotate_y.value), Y);
            o->rotate(Geometry::deg2rad(cli_config.rotate.value), Z);
        }
        
        // TODO: handle --merge
        models.push_back(model);
    }
    if (cli_config.help) {
        printUsage();
        return 0;
    }

    std::vector<SLAPrint> prints;
    for (Model &model : models) {
        if (cli_config.info) {
            // --info works on unrepaired model
            model.print_info();
        } else if (cli_config.export_obj) {
            std::string outfile = cli_config.output.value;
            if (outfile.empty()) outfile = model.objects.front()->input_file + ".obj";
    
            TriangleMesh mesh = model.mesh();
            mesh.repair();
            IO::OBJ::write(mesh, outfile);
            boost::nowide::cout << "File exported to " << outfile << std::endl;
        } else if (cli_config.export_pov) {
            std::string outfile = cli_config.output.value;
            if (outfile.empty()) outfile = model.objects.front()->input_file + ".pov";
    
            TriangleMesh mesh = model.mesh();
            mesh.repair();
            IO::POV::write(mesh, outfile);
            boost::nowide::cout << "File exported to " << outfile << std::endl;
        } else if (cli_config.export_svg) {
            std::string outfile = cli_config.output.value;
            if (outfile.empty()) 
                outfile = model.objects.front()->input_file + ".svg";
            std::cout << "Export SVG\n";

            // sentinel value for colors
            static size_t id = 0;
            
            SLAPrint print(&model, outfile, getFillColor(id)); //init print with model, fname and color
            print.config.apply(print_config, true); // apply configuration
            print.slice(); // slice file

            id++;

            // push into vector
            prints.push_back(print);
            std::cout << "Pushing into vector\n";
            //print.write_svg(outfile); // write SVG
            //boost::nowide::cout << "SVG file exported to " << outfile << std::endl;
        } else if (cli_config.export_3mf) {
            std::string outfile = cli_config.output.value;
            if (outfile.empty()) outfile = model.objects.front()->input_file;
            // Check if the file is already a 3mf.
            if(outfile.substr(outfile.find_last_of('.'), outfile.length()) == ".3mf")
                outfile = outfile.substr(0, outfile.find_last_of('.')) + "_2" + ".3mf";
            else
                // Remove the previous extension and add .3mf extention.
                outfile = outfile.substr(0, outfile.find_last_of('.')) + ".3mf";
            IO::TMF::write(model, outfile);
            boost::nowide::cout << "File file exported to " << outfile << std::endl;
        } else if (cli_config.cut_x > 0 || cli_config.cut_y > 0 || cli_config.cut > 0) {
            model.repair();
            model.translate(0, 0, -model.bounding_box().min.z);
            
            if (!model.objects.empty()) {
                // FIXME: cut all objects
                Model out;
                if (cli_config.cut_x > 0) {
                    model.objects.front()->cut(X, cli_config.cut_x, &out);
                } else if (cli_config.cut_y > 0) {
                    model.objects.front()->cut(Y, cli_config.cut_y, &out);
                } else {
                    model.objects.front()->cut(Z, cli_config.cut, &out);
                }
                
                ModelObject &upper = *out.objects[0];
                ModelObject &lower = *out.objects[1];

                // Use the input name and trim off the extension.
                std::string outfile = cli_config.output.value;
                if (outfile.empty()) outfile = model.objects.front()->input_file;
                outfile = outfile.substr(0, outfile.find_last_of('.'));
                std::cerr << outfile << "\n";
            
                if (upper.facets_count() > 0) {
                    TriangleMesh m = upper.mesh();
                    IO::STL::write(m, outfile + "_upper.stl");
                }
                if (lower.facets_count() > 0) {
                    TriangleMesh m = lower.mesh();
                    IO::STL::write(m, outfile + "_lower.stl");
                }
            }
        } else if (cli_config.cut_grid.value.x > 0 && cli_config.cut_grid.value.y > 0) {
            TriangleMesh mesh = model.mesh();
            mesh.repair();
            
            TriangleMeshPtrs meshes = mesh.cut_by_grid(cli_config.cut_grid.value);
            size_t i = 0;
            for (TriangleMesh* m : meshes) {
                std::ostringstream ss;
                ss << model.objects.front()->input_file << "_" << i++ << ".stl";
                IO::STL::write(*m, ss.str());
                delete m;
            }
        } else if (cli_config.slice) {
            std::string outfile = cli_config.output.value;
            Print print;

            model.arrange_objects(print.config.min_object_distance());
            model.center_instances_around_point(cli_config.center);
            if (outfile.empty()) outfile = model.objects.front()->input_file + ".gcode";
            print.apply_config(print_config);

            for (auto* mo : model.objects) {
                print.auto_assign_extruders(mo);
                print.add_model_object(mo);
            }
            print.validate();

            print.export_gcode(outfile);

        } else {
            boost::nowide::cerr << "error: command not supported" << std::endl;
            return 1;
        }
    }
    if (cli_config.export_svg)
    {
        // print header (1st file)
        prints[0].write_svg_header();

        size_t cur_layer = 0; // keeps track of layer nr for all layers (for print)
        size_t total_layers_size = 0; // gets all layers size;

        // get nr of layers of all models
        for (SLAPrint &print : prints)
            total_layers_size += print.get_layers_size();

        std::cout << "Total Layers Size: " << total_layers_size << std::endl;

        // print all layers to svg files alternately
        while(cur_layer < total_layers_size - 1)
        {
            for(SLAPrint &print : prints )
                print.write_svg_layer(cur_layer++);
            //std::cout << "Cur Layer: " << cur_layer << std::endl;
            //std::vector<SLAPrint>::iterator it = prints.begin();
            //for(; it != prints.end(); it++)
            //    (*it).write_svg_layer(cur_layer++);
        }
        //std::cout << "Cur layer: " << cur_layer++ << std::endl;

        // print footer (1st file)
        prints[0].write_svg_footer();
        std::string outfile = cli_config.output.value;
        boost::nowide::cout << "File file exported to " << outfile << std::endl;
    }
    
    return 0;
}
void printUsage()
{
        std::cout << "Slic3r " << SLIC3R_VERSION << " is a STL-to-GCODE translator for RepRap 3D printers" << "\n"
                  << "written by Alessandro Ranellucci <aar@cpan.org> - http://slic3r.org/ - https://github.com/slic3r/Slic3r" << "\n"
                  << "Git Version " << BUILD_COMMIT << "\n\n"
                  << "Usage (C++ only): ./slic3r [ OPTIONS ] [ file.stl ] [ file2.stl ] ..." << "\n";
        // CLI Options
        std::cout << "** CLI OPTIONS **\n";
        print_cli_options(boost::nowide::cout);
        std::cout << "****\n";
            // Print options
            std::cout << "** PRINT OPTIONS **\n";
        print_print_options(boost::nowide::cout);
        std::cout << "****\n";
}

std::string getFillColor(size_t id)
{
    return fill_clrs[ id % fill_clrs.size() ];
}

#include "osm2tempus.h"
#include "writer.h"

#include <unordered_set>

struct PbfReader
{
    void node_callback( uint64_t osmid, double lon, double lat, const osm_pbf::Tags &/*tags*/ )
    {
        points_[osmid] = Point(lon, lat);
    }

    void way_callback( uint64_t osmid, const osm_pbf::Tags& tags, const std::vector<uint64_t>& nodes )
    {
        // ignore ways that are not highway
        if ( tags.find( "highway" ) == tags.end() )
            return;

        auto r = ways.emplace( osmid, Way() );
        Way& w = r.first->second;
        w.tags = tags;
        w.nodes = nodes;
    }

    void mark_points_and_ways()
    {
        for ( auto way_it = ways.begin(); way_it != ways.end(); way_it++ ) {
            // mark each nodes as being used
            for ( uint64_t node: way_it->second.nodes ) {
                auto it = points_.find( node );
                if ( it != points_.end() ) {
                    int uses = it->second.uses();
                    if ( uses < 2 )
                        it->second.set_uses( uses + 1 );
                }
                else {
                    // unknown point
                    way_it->second.ignored = true;
                }
            }
        }
    }

    ///
    /// Convert raw OSM ways to road sections. Sections are road parts between two intersections.
    void write_sections( Writer& writer )
    {
        for ( auto way_it = ways.begin(); way_it != ways.end(); way_it++ ) {
            const Way& way = way_it->second;
            if ( way.ignored )
                continue;

            way_to_sections( way, writer );
        }
    }

    void way_to_sections( const Way& way, Writer& writer )
    {
        // split the way on intersections (i.e. node that are used more than once)
        bool section_start = true;
        uint64_t old_node = way.nodes[0];
        uint64_t node_from;
        std::vector<uint64_t> section_nodes;
        //Point old_pt = points_.find( old_node )->second;
        for ( size_t i = 1; i < way.nodes.size(); i++ ) {
            uint64_t node = way.nodes[i];
            const Point& pt = points_.find( node )->second;
            if ( section_start ) {
                section_nodes.clear();
                section_nodes.push_back( old_node );
                node_from = old_node;
                section_start = false;
            }
            section_nodes.push_back( node );
            if ( i == way.nodes.size() - 1 || pt.uses() > 1 ) {
                split_into_sections( node_from, node, section_nodes, way.tags, writer );
                //writer.write_section( node_from, node, section_pts, way.tags );
                section_start = true;
            }
            old_node = node;
        }
    }

    void relation_callback( uint64_t /*osmid*/, const osm_pbf::Tags & /*tags*/, const osm_pbf::References & /*refs*/ )
    {
    }

    const PointCache& points() const { return points_; }

private:
    PointCache points_;
    WayCache ways;

    // structure used to detect multi edges
    std::unordered_set<node_pair> way_node_pairs;
    //std::unordered_map<node_pair, uint64_t> way_node_pairs;
    uint64_t way_id = 0;

    // node ids that are introduced to split multi edges
    // we count them backward from 2^64 - 1
    // this should not overlap current OSM node ID (~ 2^32 in july 2016)
    uint64_t last_artificial_node_id = 0xFFFFFFFFFFFFFFFFLL;

        ///
    /// Check if a section with the same (from, to) pair exists
    /// In which case, it is split into two sections
    /// in order to avoid multigraphs
    void split_into_sections( uint64_t node_from, uint64_t node_to, const std::vector<uint64_t>& nodes, const osm_pbf::Tags& tags, Writer& writer )
    {
        // in order to avoid multigraphs
        node_pair p ( node_from, node_to );
        if ( way_node_pairs.find( p ) != way_node_pairs.end() ) {
            // split the way
            // if there are more than two nodes, just split on a node
            std::vector<Point> before_pts, after_pts;
            uint64_t center_node;
            if ( nodes.size() > 2 ) {
                size_t center = nodes.size() / 2;
                center_node = nodes[center];
                for ( size_t i = 0; i <= center; i++ ) {
                    before_pts.push_back( points_.find( nodes[i] )->second );
                }
                for ( size_t i = center; i < nodes.size(); i++ ) {
                    after_pts.push_back( points_.find( nodes[i] )->second );
                }
            }
            else {
                const Point& p1 = points_.find( nodes[0] )->second;
                const Point& p2 = points_.find( nodes[1] )->second;
                Point center_point( ( p1.lon() + p2.lon() ) / 2.0, ( p1.lat() + p2.lat() ) / 2.0 );
                
                before_pts.push_back( points_.find( nodes[0] )->second );
                before_pts.push_back( center_point );
                after_pts.push_back( center_point );
                after_pts.push_back( points_.find( nodes[1] )->second );

                // add a new point
                center_node = last_artificial_node_id;
                points_[last_artificial_node_id--] = center_point;
            }
            writer.write_section( node_from, center_node, before_pts, tags );
            writer.write_section( center_node, node_to, after_pts, tags );
        }
        else {
            way_node_pairs.insert( p );
            std::vector<Point> section_pts;
            for ( uint64_t node: nodes ) {
                section_pts.push_back( points_.find( node )->second );
            }
            writer.write_section( node_from, node_to, section_pts, tags );
        }
    }
};

#if 0
struct RelationReader
{
    RelationReader( const PointCache& points ) : points_( points ) {}
    void node_callback( uint64_t /*osmid*/, double /*lon*/, double /*lat*/, const osm_pbf::Tags &/*tags*/ )
    {
    }

    void way_callback( uint64_t /*osmid*/, const osm_pbf::Tags& /*tags*/, const std::vector<uint64_t>& /*nodes*/ )
    {
    }
    
    void relation_callback( uint64_t osmid, const osm_pbf::Tags & tags, const osm_pbf::References & refs )
    {
        auto r_it = tags.find( "restriction" );
        auto t_it = tags.find( "type" );
        if ( ( r_it != tags.end() ) ||
             ( t_it != tags.end() && t_it->second == "restriction" ) ) {
            uint64_t from = 0, via_n = 0, to = 0;
            for ( const osm_pbf::Reference& r : refs ) {
                bool is_node = points_.find( r.member_id ) != points_.end();
                if ( r.role == "from" && !is_node )
                    from = r.member_id;
                else if ( r.role == "via" && is_node ) {
                    via_n = r.member_id;
                }
                else if ( r.role == "to" && !is_node )
                    to = r.member_id;
            }

            if ( from && via_n && to ) {
                const Point& p = points_.find( via_n )->second;
                // emit restriction
                std::cout << r_it->second << " " << t_it->second << " " << from << " to " << to << " via " << via_n << " " << p.uses() << std::endl;
            }
        }
    }
private:
    const PointCache& points_;
};
#endif


void single_pass_pbf_read( const std::string& filename, Writer& writer )
{
    off_t ways_offset = 0, relations_offset = 0;
    osm_pbf::osm_pbf_offsets<StdOutProgressor>( filename, ways_offset, relations_offset );
    std::cout << "Ways offset: " << ways_offset << std::endl;
    std::cout << "Relations offset: " << relations_offset << std::endl;

    std::cout << "Nodes and ways ..." << std::endl;
    PbfReader p;
    osm_pbf::read_osm_pbf<PbfReader, StdOutProgressor>( filename, p, 0, relations_offset );
    p.mark_points_and_ways();
    p.write_sections( writer );

#if 0
    std::cout << "Relations ..." << std::endl;
    RelationReader r( p.points() );
    osm_pbf::read_osm_pbf<RelationReader, StdOutProgressor>( filename, r, relations_offset );
#endif
}

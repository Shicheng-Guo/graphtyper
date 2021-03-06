#include <graphtyper/graph/absolute_position.hpp>
#include <graphtyper/graph/constructor.hpp>
#include <graphtyper/graph/genomic_region.hpp>
#include <graphtyper/graph/graph_serialization.hpp>
#include <graphtyper/graph/haplotype_extractor.hpp>
#include <graphtyper/index/indexer.hpp>
#include <graphtyper/index/ph_index.hpp>
#include <graphtyper/typer/caller.hpp> // gyper::discover_directly_from_bam
#include <graphtyper/typer/primers.hpp>
#include <graphtyper/typer/variant_map.hpp>
#include <graphtyper/typer/vcf.hpp>
#include <graphtyper/typer/vcf_operations.hpp>
#include <graphtyper/utilities/bamshrink.hpp>
#include <graphtyper/utilities/genotype.hpp>
#include <graphtyper/utilities/hts_parallel_reader.hpp>
#include <graphtyper/utilities/options.hpp>
#include <graphtyper/utilities/system.hpp>

#include <paw/station.hpp>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <vector>


namespace
{

std::string
get_basename_wo_ext(std::string const & sam)
{
// Get basename without extension
  auto slash_it = std::find(sam.rbegin(), sam.rend(), '/');
  auto dot_it = std::find(sam.rbegin(), sam.rend(), '.');
  assert(dot_it != sam.rend());
  assert(std::distance(dot_it, slash_it) > 1);
  std::string reversed(dot_it + 1, slash_it);
  return std::string(reversed.rbegin(), reversed.rend());
}


} // anon namespce


namespace gyper
{

std::vector<std::string>
run_bamshrink(std::vector<std::string> const & sams,
              std::vector<std::string> const & sams_index,
              std::string const & ref_fn,
              GenomicRegion const & region,
              std::vector<double> const & avg_cov_by_readlen,
              std::string const & tmp)
{
  // Get SAM/BAM/CRAM files
  create_dir(tmp + "/bams");
  assert(sams.size() == avg_cov_by_readlen.size());
  assert(sams.size() == sams_index.size());
  GenomicRegion bs_region(region); // bs = bamshrink
  bs_region.pad(100);

  paw::Station bamshrink_station(Options::const_instance()->threads);
  std::vector<std::string> output_paths;
  output_paths.reserve(sams.size());

  for (long s = 0; s < static_cast<long>(sams.size()) - 1l; ++s)
  {
    auto const & sam = sams[s];
    auto const & avg_cov = avg_cov_by_readlen[s];
    auto const & sam_index = sams_index[s];

    std::string basename = get_basename_wo_ext(sam);
    std::ostringstream ss;
    ss << tmp << "/bams/" << basename << ".bam";
    std::string path_out = ss.str();
    output_paths.push_back(path_out);
    bamshrink_station.add_work(bamshrink,
                               bs_region.chr,
                               bs_region.begin,
                               bs_region.end,
                               sam,
                               sam_index,
                               path_out,
                               avg_cov,
                               ref_fn);
  }

  // Process the last sam on the main thread
  {
    long s = static_cast<long>(sams.size()) - 1l;
    auto const & sam = sams[s];
    auto const & avg_cov = avg_cov_by_readlen[s];
    auto const & sam_index = sams_index[s];

    std::string basename = get_basename_wo_ext(sam);
    std::ostringstream ss;
    ss << tmp << "/bams/" << basename << ".bam";
    std::string path_out = ss.str();
    output_paths.push_back(path_out);

    bamshrink_station.add_to_thread(Options::const_instance()->threads - 1,
                                    bamshrink,
                                    bs_region.chr,
                                    bs_region.begin,
                                    bs_region.end,
                                    sam,
                                    sam_index,
                                    path_out,
                                    avg_cov,
                                    ref_fn);
  }

  std::string const thread_info = bamshrink_station.join();

  // DO NOT CHANGE THIS LOG LINE (we parse it externally)
  BOOST_LOG_TRIVIAL(info) << "Finished copying data. Thread work: " << thread_info;
  // PLEEEASE

  return output_paths;
}


std::vector<std::string>
run_bamshrink_multi(std::vector<std::string> const & sams,
                    std::string const & ref_fn,
                    std::string const & interval_fn,
                    std::vector<double> const & avg_cov_by_readlen,
                    std::string const & tmp)
{
  // Get SAM/BAM/CRAM files
  create_dir(tmp + "/bams");
  assert(sams.size() == avg_cov_by_readlen.size());

  paw::Station bamshrink_station(Options::const_instance()->threads);
  std::vector<std::string> output_paths;
  output_paths.reserve(sams.size());

  for (long s = 0; s < static_cast<long>(sams.size()) - 1l; ++s)
  {
    auto const & sam = sams[s];
    auto const & avg_cov = avg_cov_by_readlen[s];
    std::string basename = get_basename_wo_ext(sam);
    std::ostringstream ss;
    ss << tmp << "/bams/" << basename << ".bam";
    std::string path_out = ss.str();
    output_paths.push_back(path_out);
    bamshrink_station.add_work(bamshrink_multi, interval_fn, sam, path_out, avg_cov, ref_fn);
  }

  // Process the last sam on the main thread
  {
    long s = static_cast<long>(sams.size()) - 1l;
    auto const & sam = sams[s];
    auto const & avg_cov = avg_cov_by_readlen[s];
    std::string basename = get_basename_wo_ext(sam);
    std::ostringstream ss;
    ss << tmp << "/bams/" << basename << ".bam";
    std::string path_out = ss.str();
    output_paths.push_back(path_out);

    bamshrink_station.add_to_thread(Options::const_instance()->threads - 1,
                                    bamshrink_multi,
                                    interval_fn,
                                    sam,
                                    path_out,
                                    avg_cov,
                                    ref_fn);
  }

  std::string thread_info = bamshrink_station.join();
  BOOST_LOG_TRIVIAL(info) << "Finished copying data. Thread work: " << thread_info;
  return output_paths;
}


void
run_samtools_merge(std::vector<std::string> & shrinked_sams, std::string const & tmp)
{
  if (Options::const_instance()->is_sam_merging_allowed &&
      Options::const_instance()->max_files_open > static_cast<long>(shrinked_sams.size()) &&
      (static_cast<long>(shrinked_sams.size()) / static_cast<long>(Options::const_instance()->threads)) >= 200l)
  {
    BOOST_LOG_TRIVIAL(info) << "Merging input files.";

    long const CHUNK_SIZE =
      std::min(10l, static_cast<long>(shrinked_sams.size() / Options::const_instance()->threads / 100l));

    long const NUM_FILES = static_cast<long>(shrinked_sams.size());
    assert(CHUNK_SIZE > 1);
    std::vector<std::string> new_shrinked_sams;
    std::vector<std::vector<std::string> > all_input_sams;
    all_input_sams.resize(NUM_FILES / CHUNK_SIZE + 1);

    {
      paw::Station merge_station(Options::const_instance()->threads);

      for (long i = 0; (i * CHUNK_SIZE) < NUM_FILES; ++i)
      {
        assert(i < static_cast<long>(all_input_sams.size()));
        std::vector<std::string> & input_sams = all_input_sams[i];
        long const file_i = i * CHUNK_SIZE;
        long const next_file_i = file_i + CHUNK_SIZE;

        if (next_file_i >= NUM_FILES)
        {
          std::copy(shrinked_sams.begin() + file_i, shrinked_sams.end(), std::back_inserter(input_sams));
        }
        else
        {
          std::copy(shrinked_sams.begin() + file_i, shrinked_sams.begin() + next_file_i,
                    std::back_inserter(input_sams));
        }

        if (input_sams.size() == 1)
        {
          // No merging needed
          new_shrinked_sams.push_back(input_sams[0]);
        }
        else if (input_sams.size() > 1)
        {
          std::ostringstream ss;
          ss << tmp << "/bams/merged" << std::setw(5) << std::setfill('0') << i << ".bam";
          new_shrinked_sams.push_back(ss.str());

          if (next_file_i < NUM_FILES)
          {
            merge_station.add_work(sam_merge, new_shrinked_sams[new_shrinked_sams.size() - 1], all_input_sams[i]);
          }
          else
          {
            // Put the very last job to the main thread
            merge_station.add_to_thread(Options::const_instance()->threads - 1,
                                        sam_merge,
                                        new_shrinked_sams[new_shrinked_sams.size() - 1],
                                        all_input_sams[i]);
          }
        }
      }

      std::string thread_info = merge_station.join();
      BOOST_LOG_TRIVIAL(info) << "Finished merging. Thread work: " << thread_info;
    }

#ifndef NDEBUG
    BOOST_LOG_TRIVIAL(debug) << "Number of merged files are " << new_shrinked_sams.size() << "\n";
#endif // NDEBUG
    shrinked_sams = std::move(new_shrinked_sams);
  }
  else
  {
    BOOST_LOG_TRIVIAL(info) << "Skipping merging step. Max files open are " <<
      Options::const_instance()->max_files_open;

    BOOST_LOG_TRIVIAL(info) << "Number of bamShrinked files are " << shrinked_sams.size()
                            << " running accross " << Options::const_instance()->threads << " threads.";
  }
}


void
genotype_only_with_a_vcf(std::string const & ref_path,
                         std::vector<std::string> const & shrinked_sams,
                         GenomicRegion const & region,
                         GenomicRegion const & padded_region,
                         Primers const * primers,
                         std::string const & tmp)
{
  // Iteration 1
  BOOST_LOG_TRIVIAL(info) << "Genotyping using an input VCF.";
  std::string const output_vcf = tmp + "/it1/final.vcf.gz";
  std::string const out_dir = tmp + "/it1";
  mkdir(out_dir.c_str(), 0755);
  bool const is_sv_graph{false};
  bool const use_absolute_positions{true};
  bool const check_index{true};
  bool const is_writing_calls_vcf{true};
  bool const is_discovery{false};
  bool const is_writing_hap{false};

  gyper::construct_graph(ref_path,
                         Options::const_instance()->vcf,
                         padded_region.to_string(),
                         is_sv_graph,
                         use_absolute_positions,
                         check_index);

  absolute_pos.calculate_offsets(gyper::graph.contigs);

#ifndef NDEBUG
  // Save graph in debug mode
  save_graph(out_dir + "/graph");
#endif // NDEBUG

  std::vector<std::string> paths;

  {
    PHIndex ph_index = index_graph(gyper::graph);

    paths = gyper::call(shrinked_sams,
                        "",                          // graph_path
                        ph_index,
                        out_dir,
                        "",                          // reference
                        ".",                          // region
                        primers,
                        5,                          //minimum_variant_support,
                        0.25,                          //minimum_variant_support_ratio,
                        is_writing_calls_vcf,
                        is_discovery,
                        is_writing_hap);
  }

  BOOST_LOG_TRIVIAL(info) << "Merging output VCFs.";

  // VCF merge and break_down
  // Append _calls.vcf.gz
  //for (auto & path : paths)
  //  path += "_calls.vcf.gz";

  //> FILTER_ZERO_QUAL, force_no_variant_overlapping, force_no_break_down
  vcf_merge_and_break(paths, tmp + "/graphtyper.vcf.gz", region.to_string(), true, false, false);

  // free memory
  graph = Graph();
}


void
genotype(std::string ref_path,
         std::vector<std::string> const & sams,
         std::vector<std::string> const & sams_index,
         GenomicRegion const & region,
         std::string const & output_path,
         std::vector<double> const & avg_cov_by_readlen,
         bool const is_copy_reference)
{
  // TODO: If the reference is only Ns then output an empty vcf with the sample names
  // TODO: Extract the reference sequence and use that to discover directly from BAM
  bool is_writing_calls_vcf{true};
  bool is_writing_hap{true};
  bool is_discovery{true};

  long minimum_variant_support = 5;
  double minimum_variant_support_ratio = 0.25;
  gyper::Options const & copts = *(Options::const_instance());

  long const NUM_SAMPLES = sams.size();
  BOOST_LOG_TRIVIAL(info) << "Genotyping region " << region.to_string();
  BOOST_LOG_TRIVIAL(info) << "Path to genome is '" << ref_path << "'";
  BOOST_LOG_TRIVIAL(info) << "Running with up to " << copts.threads << " threads.";
  BOOST_LOG_TRIVIAL(info) << "Copying data from " << NUM_SAMPLES << " input SAM/BAM/CRAMs to local disk.";

  std::string tmp = create_temp_dir(region);

  BOOST_LOG_TRIVIAL(info) << "Temporary folder is " << tmp;

  // Create directories
  mkdir(output_path.c_str(), 0755);
  mkdir((output_path + "/" + region.chr).c_str(), 0755);
  mkdir((output_path + "/input_sites").c_str(), 0755);
  mkdir((output_path + "/input_sites/" + region.chr).c_str(), 0755);

  // Copy reference genome to temporary directory
  if (is_copy_reference)
  {
    BOOST_LOG_TRIVIAL(info) << "Copying reference genome FASTA and its index to temporary folder.";

    {
      std::ostringstream ss_cmd;
      ss_cmd << "cp " << ref_path << " " << tmp << "/genome.fa";

      int ret = system(ss_cmd.str().c_str());

      if (ret != 0)
      {
        BOOST_LOG_TRIVIAL(error) << "This command failed '" << ss_cmd.str() << "'";
        std::exit(ret);
      }
    }

    // Copy reference genome index
    {
      std::ostringstream ss_cmd;
      ss_cmd << "cp " << ref_path << ".fai " << tmp << "/genome.fa.fai";

      int ret = system(ss_cmd.str().c_str());

      if (ret != 0)
      {
        BOOST_LOG_TRIVIAL(error) << "This command failed '" << ss_cmd.str() << "'";
        std::exit(ret);
      }
    }

    ref_path = tmp + "/genome.fa";
  }

  std::vector<std::string> shrinked_sams;

  if (copts.no_bamshrink)
  {
    shrinked_sams = std::move(sams);
  }
  else
  {
    std::string bamshrink_ref_path;

    if (copts.force_use_input_ref_for_cram_reading)
      bamshrink_ref_path = ref_path;

    shrinked_sams = run_bamshrink(sams, sams_index, bamshrink_ref_path, region, avg_cov_by_readlen, tmp);
    std::sort(shrinked_sams.begin(), shrinked_sams.end()); // Sort by input filename
    run_samtools_merge(shrinked_sams, tmp);
  }

  GenomicRegion padded_region(region);
  padded_region.pad(1000l);

  // Read primers from amplicon sequencing if they were specified
  std::unique_ptr<Primers> primers;

  if (copts.primer_bedpe.size() > 0)
  {
    BOOST_LOG_TRIVIAL(info) << "Reading primers from " << copts.primer_bedpe;
    primers = std::unique_ptr<Primers>(new Primers(copts.primer_bedpe));
  }

  if (copts.vcf.size() > 0)
  {
    BOOST_LOG_TRIVIAL(info) << "Genotyping a input VCF";
    genotype_only_with_a_vcf(ref_path, shrinked_sams, region, padded_region, primers.get(), tmp);
  }
  else
  {
    minimum_variant_support = copts.genotype_aln_min_support;
    minimum_variant_support_ratio = copts.genotype_aln_min_support_ratio;
    is_writing_calls_vcf = false; // Skip writing calls vcf in release mode in all iterations except the last one

    // Iteration 1
    {
      BOOST_LOG_TRIVIAL(info) << "Initial variant discovery step starting.";
      std::string const output_vcf = tmp + "/it1/final.vcf.gz";
      std::string const out_dir = tmp + "/it1";
      mkdir(out_dir.c_str(), 0755);
      gyper::construct_graph(ref_path, "", padded_region.to_string(), false, true, false);
#ifndef NDEBUG
      // Save graph in debug mode
      save_graph(out_dir + "/graph");
#endif // NDEBUG
      absolute_pos.calculate_offsets(gyper::graph.contigs);
      auto output_paths = gyper::discover_directly_from_bam("",
                                                            shrinked_sams,
                                                            padded_region.to_string(),
                                                            out_dir,
                                                            minimum_variant_support,
                                                            minimum_variant_support_ratio);
      gyper::VariantMap varmap;
      varmap.load_many_variant_maps(output_paths);
      varmap.filter_varmap_for_all();
      Vcf final_vcf;
      varmap.get_vcf(final_vcf, output_vcf);

      if (copts.prior_vcf.size() > 0)
      {
        BOOST_LOG_TRIVIAL(info) << "Inserting prior variant sites.";
        std::vector<Variant> prior_variants = get_variants_using_tabix(copts.prior_vcf, region);

        BOOST_LOG_TRIVIAL(info) << "Found " << prior_variants.size() << " prior variants.";
        std::move(prior_variants.begin(), prior_variants.end(), std::back_inserter(final_vcf.variants));
      }

      final_vcf.write(".", copts.threads);
#ifndef NDEBUG
      final_vcf.write_tbi_index(); // Write index in debug mode
#endif // NDEBUG

      // free memory
      graph = Graph();
    }

#ifndef NDEBUG
    std::string stats;

    if (Options::const_instance()->stats.size() > 0)
    {
      stats = std::move(Options::instance()->stats);
      Options::instance()->stats.clear();
    }
#endif // NDEBUG

    long FIRST_CALLONLY_ITERATION = 3;
    long LAST_ITERATION = 4;

    // Iteration 2
    if (copts.is_only_cigar_discovery)
    {
      // Skip the graphtyper discovery iteration
      --FIRST_CALLONLY_ITERATION;
      --LAST_ITERATION;
    }
    else
    {
      BOOST_LOG_TRIVIAL(info) << "Further variant discovery step starting.";
      std::string const out_dir = tmp + "/it2";
      std::string const haps_output_vcf = out_dir + "/haps.vcf.gz";
      std::string const discovery_output_vcf = out_dir + "/discovery.vcf.gz";
      mkdir(out_dir.c_str(), 0755);
      construct_graph(ref_path, tmp + "/it1/final.vcf.gz", padded_region.to_string(), false, true, false);

#ifndef NDEBUG
      // Save graph in debug mode
      save_graph(out_dir + "/graph");
#endif // NDEBUG

      std::vector<std::string> paths;

      {
        PHIndex ph_index = index_graph(gyper::graph);

        minimum_variant_support = copts.genotype_dis_min_support;
        minimum_variant_support_ratio = copts.genotype_dis_min_support_ratio;

        paths = gyper::call(shrinked_sams,
                            "", // graph_path
                            ph_index,
                            out_dir,
                            "", // reference
                            ".", // region
                            nullptr,//primers.get(),
                            minimum_variant_support,
                            minimum_variant_support_ratio,
                            is_writing_calls_vcf,
                            is_discovery,
                            is_writing_hap);
      }

      Vcf haps_vcf;
      extract_to_vcf(haps_vcf,
                     paths,
                     haps_output_vcf,
                     true); // is_splitting_vars

      // Append _variant_map
      for (auto & path : paths)
        path += "_variant_map";

      VariantMap varmap;
      varmap.load_many_variant_maps(paths);
      varmap.filter_varmap_for_all();

      Vcf discovery_vcf;
      varmap.get_vcf(discovery_vcf, out_dir + "/final.vcf.gz");
      std::move(haps_vcf.variants.begin(), haps_vcf.variants.end(), std::back_inserter(discovery_vcf.variants));
      discovery_vcf.write(".", copts.threads);

#ifndef NDEBUG
      discovery_vcf.write_tbi_index(); // Write index in debug mode
#endif // NDEBUG

      // free memory
      graph = Graph();
    }

    is_discovery = false; // No more discovery
    std::vector<std::string> paths;

    // Iteration FIRST_CALLONLY_ITERATION-LAST_ITERATION
    for (long i = FIRST_CALLONLY_ITERATION; i <= LAST_ITERATION; ++i)
    {
      BOOST_LOG_TRIVIAL(info) << "Call step " << (i - FIRST_CALLONLY_ITERATION + 1) << " starting.";

      if (i == LAST_ITERATION)
      {
        is_writing_calls_vcf = true; // Always write calls vcf in the last iteration
        is_writing_hap = false; // No need for writing .hap
      }

      std::string prev_out_vcf;
      std::string out_dir;

      {
        std::ostringstream ss_prev;
        ss_prev << tmp << "/it" << (i - 1) << "/final.vcf.gz";
        prev_out_vcf = ss_prev.str();

        std::ostringstream ss;
        ss << tmp << "/it" << i;
        out_dir = ss.str();
      }

      mkdir(out_dir.c_str(), 0755);
      std::string const haps_output_vcf = out_dir + "/final.vcf.gz";
      construct_graph(ref_path, prev_out_vcf, padded_region.to_string(), false, true, false);

#ifndef NDEBUG
      // Save graph in debug mode
      save_graph(out_dir + "/graph");
#endif // NDEBUG

      {
        PHIndex ph_index = index_graph(gyper::graph);

        paths = gyper::call(shrinked_sams,
                            "", // graph_path
                            ph_index,
                            out_dir,
                            "", // reference
                            ".", // region
                            primers.get(),
                            minimum_variant_support,
                            minimum_variant_support_ratio,
                            is_writing_calls_vcf,
                            is_discovery,
                            is_writing_hap);
      }

      if (i < LAST_ITERATION)
      {
        // Split variants unless its the next-to-last iteration
        bool const is_splitting_vars = (i + 1) < LAST_ITERATION;

        Vcf haps_vcf;
        extract_to_vcf(haps_vcf,
                       paths,
                       haps_output_vcf,
                       is_splitting_vars);

        haps_vcf.write(".", copts.threads);
#ifndef NDEBUG
        // Write index in debug mode
        haps_vcf.write_tbi_index();
#endif // NDEBUG

        // free memory
        graph = Graph();
      }
    }

    BOOST_LOG_TRIVIAL(info) << "Merging output VCFs.";

    // VCF merge and break_down
    {
      // Append _calls.vcf.gz
      //for (auto & path : paths)
      //  path += "_calls.vcf.gz";

      //> FILTER_ZERO_QUAL, force_no_variant_overlapping
      vcf_merge_and_break(paths, tmp + "/graphtyper.vcf.gz", region.to_string(), true, false, false);

      if (copts.normal_and_no_variant_overlapping)
      {
        //> FILTER_ZERO_QUAL, force_no_variant_overlapping
        vcf_merge_and_break(paths,
                            tmp + "/graphtyper.no_variant_overlapping.vcf.gz",
                            region.to_string(),
                            true,
                            true,
                            false);
      }
    }


    BOOST_LOG_TRIVIAL(info) << "Copying results to output directory.";

    // Copy sites to system
    {
      std::ostringstream ss_cmd;
      ss_cmd << "cp -p " << tmp << "/it" << (LAST_ITERATION - 1) << "/final.vcf.gz" << " " // Change to (LAST_ITERATION - 1)
             << output_path << "/input_sites/" << region.chr << "/"
             << std::setw(9) << std::setfill('0') << (region.begin + 1)
             << '-'
             << std::setw(9) << std::setfill('0') << region.end
             << ".vcf.gz";

      int ret = system(ss_cmd.str().c_str());

      if (ret != 0)
      {
        BOOST_LOG_TRIVIAL(error) << "This command failed '" << ss_cmd.str() << "'";
        std::exit(ret);
      }
    }
  }

  // Copy final VCFs
  auto copy_to_results =
    [&](std::string const & basename_no_ext, std::string const & extension, std::string const & id)
    {
      std::ostringstream ss_cmd;
      ss_cmd << "cp -p " << tmp << "/" << basename_no_ext << extension << " "
             << output_path << "/" << region.chr << "/"
             << std::setw(9) << std::setfill('0') << (region.begin + 1)
             << '-'
             << std::setw(9) << std::setfill('0') << region.end
             << id
             << extension;

      int ret = system(ss_cmd.str().c_str());

      if (extension != ".vcf.gz.tbi" && ret != 0)
      {
        BOOST_LOG_TRIVIAL(error) << __HERE__ << " This command failed '" << ss_cmd.str() << "'";
        std::exit(ret);
      }
      else if (ret != 0)
      {
        // Just a warning if tabix fails and there is no index
        BOOST_LOG_TRIVIAL(warning) << __HERE__ << " This command failed '" << ss_cmd.str() << "'";
      }
    };

  copy_to_results("graphtyper", ".vcf.gz", ""); // Copy final VCF
  copy_to_results("graphtyper", ".vcf.gz.tbi", ""); // Copy tabix index for final VCF

  if (copts.normal_and_no_variant_overlapping)
  {
    copy_to_results("graphtyper.no_variant_overlapping", ".vcf.gz", ".no_variant_overlapping");
    copy_to_results("graphtyper.no_variant_overlapping", ".vcf.gz.tbi", ".no_variant_overlapping");
  }

  if (!copts.no_cleanup)
  {
    BOOST_LOG_TRIVIAL(info) << "Cleaning up temporary files.";
    remove_file_tree(tmp.c_str());
  }
  else
  {
    BOOST_LOG_TRIVIAL(info) << "Temporary files left: " << tmp;
  }

  {
    std::ostringstream ss;
    ss << output_path << "/" << region.chr << "/"
       << std::setw(9) << std::setfill('0') << (region.begin + 1)
       << '-'
       << std::setw(9) << std::setfill('0') << region.end
       << ".vcf.gz";

    BOOST_LOG_TRIVIAL(info) << "Finished! Output written at: " << ss.str();
  }
}


void
genotype_regions(std::string const & ref_path,
                 std::vector<std::string> const & sams,
                 std::vector<std::string> const & sams_index,
                 std::vector<GenomicRegion> const & regions,
                 std::string const & output_path,
                 std::vector<double> const & avg_cov_by_readlen,
                 bool const is_copy_reference)
{
  auto & opts = *(gyper::Options::instance());
  long const NUM_SAMPLES = sams.size();

  // parameter adjustment based on cohort size
  // default aln: 4 and 0.21, dis:0.30 and 8, ext: 9 and 2
  if (NUM_SAMPLES >= 4)
  {
    // default aln: 5 and 0.23, dis:0.30 and 9, ext: 15 and 2
    ++opts.genotype_aln_min_support;
    ++opts.genotype_dis_min_support;
    opts.genotype_aln_min_support_ratio += 0.02;
    opts.minimum_extract_score_over_homref += 6;

    if (NUM_SAMPLES >= 100)
    {
      // default aln:6 and 0.25, dis:0.30 and 10, ext: 21 and 2
      ++opts.genotype_aln_min_support;
      ++opts.genotype_dis_min_support;
      opts.genotype_aln_min_support_ratio += 0.02;
      opts.minimum_extract_score_over_homref += 6;

      if (NUM_SAMPLES >= 500)
      {
        // default aln:7 and 0.26, ext: 27 and 2
        ++opts.genotype_aln_min_support;
        ++opts.genotype_dis_min_support;
        opts.genotype_aln_min_support_ratio += 0.01;
        opts.minimum_extract_score_over_homref += 6;

        if (NUM_SAMPLES >= 10000)
        {
          // default aln:8 and 0.27, ext:30 and 3
          ++opts.genotype_aln_min_support;
          ++opts.genotype_dis_min_support;
          opts.genotype_aln_min_support_ratio += 0.01;
          opts.genotype_dis_min_support_ratio += 0.01;
          ++opts.minimum_extract_variant_support;
          opts.minimum_extract_score_over_homref += 3;
        }
      }
    }
  }

  // Genotype regions serially
  for (auto const & region : regions)
  {
    genotype(ref_path,
             sams,
             sams_index,
             region,
             output_path,
             avg_cov_by_readlen,
             is_copy_reference);
  }
}


} // namespace gyper

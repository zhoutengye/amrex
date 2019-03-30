#include <cuda_runtime.h>

#include <iostream>
#include <AMReX.H>
#include <AMReX_Print.H>
#include <AMReX_Geometry.H>
#include <AMReX_Vector.H>
#include <AMReX_ParmParse.H>
#include <AMReX_MultiFab.H>
#include <AMReX_Gpu.H>

#include <Kokkos_Core.hpp>

// Current timers:
// Ignore resetting of value and output. All else included.


using namespace amrex;

// &&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

int main (int argc, char* argv[])
{

    amrex::Initialize(argc, argv); {
    {
        Kokkos::InitArguments args;
        args.device_id = amrex::Gpu::Device::deviceId();

        Kokkos::initialize(args);
    }
    {

        amrex::Print() << "amrex::Initialize complete." << "\n";

        // ===================================
        // Simple cuda action to make sure all tests have cuda.
        // Allows nvprof to return data.
        int devices = 0;
#ifdef AMREX_USE_CUDA
        cudaGetDeviceCount(&devices);
#endif
        amrex::Print() << "Hello world from AMReX version " << amrex::Version() << ". GPU devices: " << devices << "\n";
        amrex::Print() << "**********************************\n"; 
        // ===================================

        // What time is it now?  We'll use this to compute total run time.
        Real strt_time = amrex::second();

        // AMREX_SPACEDIM: number of dimensions
        int n_cell, max_grid_size;
        Vector<int> is_periodic(AMREX_SPACEDIM,1);  // periodic in all direction by default

        // inputs parameters
        {
            // ParmParse is way of reading inputs from the inputs file
            ParmParse pp;

            // We need to get n_cell from the inputs file - this is the number of cells on each side of 
            //   a square (or cubic) domain.
            pp.get("n_cell",n_cell);

            // The domain is broken into boxes of size max_grid_size
            pp.get("max_grid_size",max_grid_size);
        }

        // make BoxArray and Geometry
        BoxArray ba;
        Geometry geom;
        {
            IntVect dom_lo(AMREX_D_DECL(       0,        0,        0));
            IntVect dom_hi(AMREX_D_DECL(n_cell-1, n_cell-1, n_cell-1));
            Box domain(dom_lo, dom_hi);

            // Initialize the boxarray "ba" from the single box "bx"
            ba.define(domain);
            // Break up boxarray "ba" into chunks no larger than "max_grid_size" along a direction
            ba.maxSize(max_grid_size);

            // This defines the physical box, [-1,1] in each direction.
            RealBox real_box({AMREX_D_DECL(-1.0,-1.0,-1.0)},
                             {AMREX_D_DECL( 1.0, 1.0, 1.0)});

            // This defines a Geometry object
            geom.define(domain,&real_box,CoordSys::cartesian,is_periodic.data());
        }

        // Nghost = number of ghost cells for each array 
        int Nghost = 1;
    
        // Ncomp = number of components for each array
        int Ncomp  = 1;
  
        // How Boxes are distrubuted among MPI processes
        DistributionMapping dm(ba);

        // Malloc value for setval testing.
        Real* val;
        cudaMallocHost(&val, sizeof(Real));
        *val = 0.25;

        // Create the MultiFab and touch the data.
        // Ensures the data in on the GPU for all further testing.
        MultiFab x(ba, dm, Ncomp, Nghost);
        x.setVal(*val);

        // ===================

        // Version with setable options.
//        using memory = Kokkos::Cuda;
//        using memory = Kokkos::CudaUVMSpace; 

//        using layout = Kokkos::LayoutLeft;
//        using layout = Kokkos::LayoutRight;

//        Kokkos::View<double***, layout, memory> view3D ("3D", n_cell,n_cell,n_cell);
//        Kokkos::View<double*,   layout, memory> view1D ("1D", n_cell*n_cell*n_cell);

        // Default memory spaces.
        Kokkos::View<double*>   view1D ("1D", n_cell*n_cell*n_cell);
        Kokkos::View<double***> view3D ("3D", n_cell,n_cell,n_cell);

        Kokkos::parallel_for(Kokkos::RangePolicy<>(0,n_cell*n_cell*n_cell),
        KOKKOS_LAMBDA (int i)
        {
            view1D(i) = *val;
        });

        Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({0,0,0},{n_cell,n_cell,n_cell}),
        KOKKOS_LAMBDA (int i, int j, int k)
        {
            view3D(i,j,k) = *val;
        });

        // ===================

        amrex::Print() << "MultiFab initialized with: " << n_cell << "^3, max_box_length = " << max_grid_size
                       << std::endl << std::endl;

        Real points = ba.numPts();

// &&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&
//      Initial launch to remove any unknown costs in HtoD setup. 

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
                // ..................
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                const auto lo = amrex::lbound(bx);
                const auto hi = amrex::ubound(bx);

                BL_PROFILE_VAR("Kokkos One",ko);

                Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({lo.x,lo.y,lo.z},{hi.x+1,hi.y+1,hi.z+1}),
                KOKKOS_LAMBDA (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(ko);

                // ..................

            }
        }

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                BL_PROFILE_VAR("Amrex One",ao);

                amrex::ParallelFor(bx,
                [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(ao);
            }

        }

        {
            BL_PROFILE("Kokkos View 3D One");

            Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({0,0,0},{n_cell,n_cell,n_cell}),
            KOKKOS_LAMBDA (int i, int j, int k)
            {
                view3D(i,j,k) = *val;
            });
        }

        {
            BL_PROFILE("Kokkos View 1D One");

            Kokkos::parallel_for(Kokkos::RangePolicy<>(0,n_cell*n_cell*n_cell),
            KOKKOS_LAMBDA (int i)
            {
                view1D(i) = *val;
            });
        }

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
                // ..................
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                const auto lo = amrex::lbound(bx);
                const auto hi = amrex::ubound(bx);

                BL_PROFILE_VAR("Kokkos Two",koo);

                Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({lo.x,lo.y,lo.z},{hi.x+1,hi.y+1,hi.z+1}),
                KOKKOS_LAMBDA (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(koo);

                // ..................

            }

        }

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
 
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                BL_PROFILE_VAR("Amrex Two",aoo);

                amrex::ParallelFor(bx,
                [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(aoo);
            }

        }

        {
            BL_PROFILE("Kokkos View 3D Two");

            Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({0,0,0},{n_cell,n_cell,n_cell}),
            KOKKOS_LAMBDA (int i, int j, int k)
            {
                view3D(i,j,k) = *val;
            });
        }

        {
            BL_PROFILE("Kokkos View 1D Two");

            Kokkos::parallel_for(Kokkos::RangePolicy<>(0,n_cell*n_cell*n_cell),
            KOKKOS_LAMBDA (int i)
            {
                view1D(i) = *val;
            });
        }

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
                // ..................
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                const auto lo = amrex::lbound(bx);
                const auto hi = amrex::ubound(bx);

                BL_PROFILE_VAR("Kokkos Three", kooo);

                Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({lo.x,lo.y,lo.z},{hi.x+1,hi.y+1,hi.z+1}),
                KOKKOS_LAMBDA (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(kooo);

                // ..................

            }

        }

        {
            for (MFIter mfi(x); mfi.isValid(); ++mfi)
            {
 
                const Box bx = mfi.validbox();
                Array4<Real> a = x.array(mfi);

                BL_PROFILE_VAR("Amrex Three", aooo);

                amrex::ParallelFor(bx,
                [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    a(i,j,k) += *val;
                });

                BL_PROFILE_VAR_STOP(aooo);
            }

        }

        {
            BL_PROFILE("Kokkos View 3D Three");

            Kokkos::parallel_for(Kokkos::MDRangePolicy<Kokkos::Rank<3>>({0,0,0},{n_cell,n_cell,n_cell}),
            KOKKOS_LAMBDA (int i, int j, int k)
            {
                view3D(i,j,k) = *val;
            });
        }

        {
            BL_PROFILE("Kokkos View 1D Three");

            Kokkos::parallel_for(Kokkos::RangePolicy<>(0,n_cell*n_cell*n_cell),
            KOKKOS_LAMBDA (int i)
            {
                view1D(i) = *val;
            });
        }



// &&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&

    amrex::Print() << "Test Completed." << std::endl;

    } Kokkos::finalize();
    } amrex::Finalize();

}
/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2015-2020 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "phaseSystem.H"
#include "surfaceTensionModel.H"
#include "aspectRatioModel.H"
#include "surfaceInterpolate.H"
#include "fvcDdt.H"
#include "localEulerDdtScheme.H"

#include "dragModel.H"
#include "BlendedInterfacialModel.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(phaseSystem, 0);
}

const Foam::word Foam::phaseSystem::propertiesName("phaseProperties");


// * * * * * * * * * * * * Protected Member Functions  * * * * * * * * * * * //

Foam::tmp<Foam::surfaceScalarField> Foam::phaseSystem::calcPhi
(
    const phaseModelList& phaseModels
) const
{
    tmp<surfaceScalarField> tmpPhi
    (
        surfaceScalarField::New
        (
            "phi",
            fvc::interpolate(phaseModels[0])*phaseModels[0].phi()
        )
    );

    for (label phasei=1; phasei<phaseModels.size(); phasei++)
    {
        tmpPhi.ref() +=
            fvc::interpolate(phaseModels[phasei])*phaseModels[phasei].phi();
    }

    return tmpPhi;
}


void Foam::phaseSystem::generatePairs
(
    const dictTable& modelDicts
)
{
    forAllConstIter(dictTable, modelDicts, iter)
    {
        const phasePairKey& key = iter.key();

        // pair already exists
        if (phasePairs_.found(key))
        {}

        // new ordered pair
        else if (key.ordered())
        {
            phasePairs_.insert
            (
                key,
                autoPtr<phasePair>
                (
                    new orderedPhasePair
                    (
                        phaseModels_[key.first()],
                        phaseModels_[key.second()]
                    )
                )
            );
        }

        // new unordered pair
        else
        {
            phasePairs_.insert
            (
                key,
                autoPtr<phasePair>
                (
                    new phasePair
                    (
                        phaseModels_[key.first()],
                        phaseModels_[key.second()]
                    )
                )
            );
        }
    }
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::phaseSystem::phaseSystem
(
    const fvMesh& mesh
)
:
    IOdictionary
    (
        IOobject
        (
            "phaseProperties",
            mesh.time().constant(),
            mesh,
            IOobject::MUST_READ_IF_MODIFIED,
            IOobject::NO_WRITE
        )
    ),

    mesh_(mesh),

    phaseModels_(lookup("phases"), phaseModel::iNew(*this)),

    phi_(calcPhi(phaseModels_)),

    dpdt_
    (
        IOobject
        (
            "dpdt",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar(dimPressure/dimTime, 0)
    ),

    MRF_(mesh_)
{
    // Groupings
    label movingPhasei = 0;
    label stationaryPhasei = 0;
    label anisothermalPhasei = 0;
    label multiComponentPhasei = 0;
    forAll(phaseModels_, phasei)
    {
        phaseModel& phase = phaseModels_[phasei];
        movingPhasei += !phase.stationary();
        stationaryPhasei += phase.stationary();
        anisothermalPhasei += !phase.isothermal();
        multiComponentPhasei += !phase.pure();
    }
    movingPhaseModels_.resize(movingPhasei);
    stationaryPhaseModels_.resize(stationaryPhasei);
    anisothermalPhaseModels_.resize(anisothermalPhasei);
    multiComponentPhaseModels_.resize(multiComponentPhasei);

    movingPhasei = 0;
    stationaryPhasei = 0;
    anisothermalPhasei = 0;
    multiComponentPhasei = 0;
    forAll(phaseModels_, phasei)
    {
        phaseModel& phase = phaseModels_[phasei];
        if (!phase.stationary())
        {
            movingPhaseModels_.set(movingPhasei++, &phase);
        }
        if (phase.stationary())
        {
            stationaryPhaseModels_.set(stationaryPhasei++, &phase);
        }
        if (!phase.isothermal())
        {
            anisothermalPhaseModels_.set(anisothermalPhasei++, &phase);
        }
        if (!phase.pure())
        {
            multiComponentPhaseModels_.set(multiComponentPhasei++, &phase);
        }
    }

    // Write phi
    phi_.writeOpt() = IOobject::AUTO_WRITE;

    // Blending methods
    forAllConstIter(dictionary, subDict("blending"), iter)
    {
        blendingMethods_.insert
        (
            iter().keyword(),
            blendingMethod::New
            (
                iter().keyword(),
                iter().dict(),
                phaseModels_.toc()
            )
        );
    }

    // Sub-models
    generatePairsAndSubModels("surfaceTension", surfaceTensionModels_);
    generatePairsAndSubModels("aspectRatio", aspectRatioModels_);

    // Update motion fields
    correctKinematics();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::phaseSystem::~phaseSystem()
{}


// * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * * //

Foam::tmp<Foam::volScalarField> Foam::phaseSystem::rho() const
{
    tmp<volScalarField> rho(movingPhaseModels_[0]*movingPhaseModels_[0].rho());

    for
    (
        label movingPhasei=1;
        movingPhasei<movingPhaseModels_.size();
        movingPhasei++
    )
    {
        rho.ref() +=
            movingPhaseModels_[movingPhasei]
           *movingPhaseModels_[movingPhasei].rho();
    }

    if (stationaryPhaseModels_.empty())
    {
        return rho;
    }

    volScalarField sumAlphaMoving
    (
        volScalarField::New
        (
            "sumAlphaMoving",
            movingPhaseModels_[0],
            calculatedFvPatchScalarField::typeName
        )
    );

    for
    (
        label movingPhasei=1;
        movingPhasei<movingPhaseModels_.size();
        movingPhasei++
    )
    {
        sumAlphaMoving += movingPhaseModels_[movingPhasei];
    }

    return rho/sumAlphaMoving;
}


Foam::tmp<Foam::volVectorField> Foam::phaseSystem::U() const
{
    tmp<volVectorField> U(movingPhaseModels_[0]*movingPhaseModels_[0].U());

    for
    (
        label movingPhasei=1;
        movingPhasei<movingPhaseModels_.size();
        movingPhasei++
    )
    {
        U.ref() +=
            movingPhaseModels_[movingPhasei]
           *movingPhaseModels_[movingPhasei].U();
    }

    if (stationaryPhaseModels_.empty())
    {
        return U;
    }

    volScalarField sumAlphaMoving
    (
        volScalarField::New
        (
            "sumAlphaMoving",
            movingPhaseModels_[0],
            calculatedFvPatchScalarField::typeName
        )
    );

    for
    (
        label movingPhasei=1;
        movingPhasei<movingPhaseModels_.size();
        movingPhasei++
    )
    {
        sumAlphaMoving += movingPhaseModels_[movingPhasei];
    }

    return U/sumAlphaMoving;
}


Foam::tmp<Foam::volScalarField>
Foam::phaseSystem::E(const phasePairKey& key) const
{
    if (aspectRatioModels_.found(key))
    {
        return aspectRatioModels_[key]->E();
    }
    else
    {
        return volScalarField::New
        (
            aspectRatioModel::typeName + ":E",
            mesh_,
            dimensionedScalar(dimless, 1)
        );
    }
}


Foam::tmp<Foam::volScalarField>
Foam::phaseSystem::sigma(const phasePairKey& key) const
{
    if (surfaceTensionModels_.found(key))
    {
        return surfaceTensionModels_[key]->sigma();
    }
    else
    {
        return volScalarField::New
        (
            surfaceTensionModel::typeName + ":sigma",
            mesh_,
            dimensionedScalar(surfaceTensionModel::dimSigma, 0)
        );
    }
}


Foam::tmp<Foam::scalarField>
Foam::phaseSystem::sigma(const phasePairKey& key, label patchi) const
{
    if (surfaceTensionModels_.found(key))
    {
        return surfaceTensionModels_[key]->sigma(patchi);
    }
    else
    {
        return tmp<scalarField>
        (
            new scalarField(mesh_.boundary()[patchi].size(), 0)
        );
    }
}


Foam::tmp<Foam::volScalarField> Foam::phaseSystem::dmdtf
(
    const phasePairKey& key
) const
{
    const phasePair pair
    (
        phaseModels_[key.first()],
        phaseModels_[key.second()]
    );

    return volScalarField::New
    (
        IOobject::groupName("dmdtf", pair.name()),
        mesh(),
        dimensionedScalar(dimDensity/dimTime, 0)
    );
}


Foam::PtrList<Foam::volScalarField> Foam::phaseSystem::dmdts() const
{
    return PtrList<volScalarField>(phaseModels_.size());
}


bool Foam::phaseSystem::incompressible() const
{
    forAll(phaseModels_, phasei)
    {
        if (!phaseModels_[phasei].incompressible())
        {
            return false;
        }
    }

    return true;
}


bool Foam::phaseSystem::implicitPhasePressure(const phaseModel& phase) const
{
    return false;
}


bool Foam::phaseSystem::implicitPhasePressure() const
{
    return false;
}


void Foam::phaseSystem::solve
(
    const PtrList<volScalarField>& rAUs,
    const PtrList<surfaceScalarField>& rAUfs
)
{}


void Foam::phaseSystem::correct()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correct();
    }
}


void Foam::phaseSystem::correctContinuityError()
{
    const PtrList<volScalarField> dmdts = this->dmdts();

    forAll(movingPhaseModels_, movingPhasei)
    {
        phaseModel& phase = movingPhaseModels_[movingPhasei];
        const volScalarField& alpha = phase;
        volScalarField& rho = phase.thermoRef().rho();

        volScalarField source
        (
            volScalarField::New
            (
                IOobject::groupName("source", phase.name()),
                mesh_,
                dimensionedScalar(dimDensity/dimTime, 0)
            )
        );

        if (fvOptions().appliesToField(rho.name()))
        {
            source += fvOptions()(alpha, rho)&rho;
        }

        if (dmdts.set(phase.index()))
        {
            source += dmdts[phase.index()];
        }

        phase.correctContinuityError(source);
    }
}


void Foam::phaseSystem::correctKinematics()
{
    bool updateDpdt = false;

    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctKinematics();

        updateDpdt = updateDpdt || phaseModels_[phasei].thermo().dpdt();
    }

    // Update the pressure time-derivative if required
    if (updateDpdt)
    {
        dpdt_ = fvc::ddt(phaseModels_.begin()().thermo().p());
    }
}


void Foam::phaseSystem::correctThermo()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctThermo();
    }
}


void Foam::phaseSystem::correctReactions()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctReactions();
    }
}


void Foam::phaseSystem::correctSpecies()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctSpecies();
    }
}


void Foam::phaseSystem::correctTurbulence()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctTurbulence();
    }
}


void Foam::phaseSystem::correctEnergyTransport()
{
    forAll(phaseModels_, phasei)
    {
        phaseModels_[phasei].correctEnergyTransport();
    }
}


bool Foam::phaseSystem::read()
{
    if (regIOobject::read())
    {
        bool readOK = true;

        forAll(phaseModels_, phasei)
        {
            readOK &= phaseModels_[phasei].read();
        }

        // models ...

        return readOK;
    }
    else
    {
        return false;
    }
}


Foam::tmp<Foam::volScalarField> Foam::byDt(const volScalarField& vf)
{
    if (fv::localEulerDdt::enabled(vf.mesh()))
    {
        return fv::localEulerDdt::localRDeltaT(vf.mesh())*vf;
    }
    else
    {
        return vf/vf.mesh().time().deltaT();
    }
}


Foam::tmp<Foam::surfaceScalarField> Foam::byDt(const surfaceScalarField& sf)
{
    if (fv::localEulerDdt::enabled(sf.mesh()))
    {
        return fv::localEulerDdt::localRDeltaTf(sf.mesh())*sf;
    }
    else
    {
        return sf/sf.mesh().time().deltaT();
    }
}


// ************************************************************************* //
